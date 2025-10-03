#ifndef __EMSCRIPTEN__
#include "replication/backends/client_tcp.h"
#include "replication/repl_iface.h"
#include "replication/repl_types.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/wire_tcp.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

/* ===== HELO/WLCM (тот же формат, что в leader_tcp.c) ===== */
#define HS_MAGIC_HELO 0x4F4C4548u /* 'HELO' LE */
#define HS_MAGIC_WLCM 0x4D434C57u /* 'WLCM' LE */
#define HS_VER        1u
typedef struct {
    uint32_t magic;   /* HELO */
    uint16_t ver;     /* 1 */
    uint16_t _rsv;
    uint64_t console_id;
} HelloPkt;                   /* 16 bytes */
typedef HelloPkt WelcomePkt;  /* magic=WLCM */

#ifndef CLIENT_MAX_LISTENERS
#  define CLIENT_MAX_LISTENERS 16
#endif
#ifndef CLIENT_MAX_BUFFERED
#  define CLIENT_MAX_BUFFERED 128
#endif

typedef struct Listener {
    ReplicatorConfirmCb cb;
    void* user;
    uint64_t cid; /* фильтр по op->console_id; 0 = wildcard */
} Listener;

enum CliState { ST_IDLE=0, ST_CONNECTING, ST_HELO_WR, ST_WLCM_RD, ST_STREAM };

typedef struct {
    NetPoller* np;
    uint64_t   console_id;
    char       host[256];
    uint16_t   port;
    enum CliState st;
    net_fd_t   fd;
    size_t     out_off, out_len;
    uint8_t    out_buf[sizeof(HelloPkt)];
    size_t     in_got;
    uint8_t    in_buf[sizeof(WelcomePkt)];
    Cow1Tcp*   cow; /* в ST_STREAM */

    /* локальные слушатели */
    Listener   ls[CLIENT_MAX_LISTENERS];
    int        ln;

    /* небольшой буфер publish до установления STREAM (копии ConOp payload’ов) */
    ConOp      q[CLIENT_MAX_BUFFERED];
    int        qn;
} CliImpl;

/* ===== локальная доставка подтверждений ===== */
static void fanout_local(CliImpl* r, const ConOp* op){
    if (!r || !op) return;
    for (int i=0;i<r->ln;i++){
        if (r->ls[i].cb && (r->ls[i].cid==0 || r->ls[i].cid==op->console_id)){
            r->ls[i].cb(r->ls[i].user, op);
        }
    }
}

/* ===== util ===== */
static void free_op_payloads(ConOp* o){
    if (!o) return;
    /* мы буферизуем/копируем только data/init; tag — на стеке/константа */
    if (o->data){ free((void*)o->data); o->data=NULL; o->size=0; }
    if (o->init_blob){ free((void*)o->init_blob); o->init_blob=NULL; o->init_size=0; }
}
static void queue_op(CliImpl* c, const ConOp* op){
    if (c->qn >= CLIENT_MAX_BUFFERED) return;
    ConOp t = *op;
    if (op->data && op->size){ void* d=malloc(op->size); if(d){ memcpy(d,op->data,op->size); t.data=d; t.size=op->size; } }
    if (op->init_blob && op->init_size){
        void* i=malloc(op->init_size);
        if(i){ memcpy(i,op->init_blob,op->init_size); t.init_blob=i; t.init_size=op->init_size; }
    }
    c->q[c->qn++] = t;
}
static void flush_queue(CliImpl* c){
    if (!c || !c->cow) return;
    for (int i=0;i<c->qn;i++){
        cow1tcp_send(c->cow, &c->q[i]);
        free_op_payloads(&c->q[i]);
    }
    c->qn = 0;
}

/* ===== события net poller ===== */
static void cli_to_idle(CliImpl* c){
    if (!c) return;
    if (c->cow){ cow1tcp_destroy(c->cow); c->cow=NULL; }
    if ((intptr_t)c->fd >= 0){ net_poller_del(c->np, c->fd); tcp_close((tcp_fd_t)c->fd); }
    c->fd = (net_fd_t)(intptr_t)-1;
    c->st = ST_IDLE;
    c->out_off = c->out_len = 0;
    c->in_got = 0;
}

static void on_op_from_server(void* user,
                              const ConOp* inop,
                              const char* tag,
                              const void* data, size_t dlen,
                              const void* init, size_t ilen)
{
    (void)tag;
    CliImpl* c = (CliImpl*)user; if (!c || !inop) return;
    /* скопировать payload’ы — dec освобождает свои */
    ConOp op = *inop;
    void* d=NULL; void* i=NULL;
    if (data && dlen){ d=malloc(dlen); if(d){ memcpy(d,data,dlen); op.data=d; op.size=dlen; } }
    if (init && ilen){ i=malloc(ilen); if(i){ memcpy(i,init,ilen); op.init_blob=i; op.init_size=ilen; } }
    fanout_local(c, &op);
    free(d); free(i);
}

static void on_connect_cb(void* user, net_fd_t fd, int ev){
    CliImpl* c = (CliImpl*)user; if (!c) return;
    if (!(ev & (NET_WR|NET_ERR))) return;
    if (net_connect_finished(fd) != NET_OK){ cli_to_idle(c); return; }
    /* готово — сформировать HELO и переход в HELO_WR */
    uint8_t out[sizeof(HelloPkt)];
    HelloPkt hp; hp.magic=HS_MAGIC_HELO; hp.ver=HS_VER; hp._rsv=0; hp.console_id=c->console_id;
    memcpy(out, &hp, sizeof(hp));
    memcpy(c->out_buf, out, sizeof(out));
    c->out_off=0; c->out_len=sizeof(out);
    c->st = ST_HELO_WR;
    net_poller_mod(c->np, fd, NET_WR|NET_ERR);
}

static void on_hs_cb(void* user, net_fd_t fd, int ev){
    CliImpl* c = (CliImpl*)user; if (!c) return;
    if (ev & NET_ERR){ cli_to_idle(c); return; }
    if (c->st == ST_HELO_WR && (ev & NET_WR)){
        size_t left = c->out_len - c->out_off;
        if (left){
            size_t wrote = 0;
            int rc = tcp_write_all((tcp_fd_t)fd, c->out_buf + c->out_off, left, &wrote);
            c->out_off += wrote;
            if (rc < 0){ cli_to_idle(c); return; }
        }
        if (c->out_off >= c->out_len){
            c->st = ST_WLCM_RD;
            c->in_got = 0;
            net_poller_mod(c->np, fd, NET_RD|NET_ERR);
        }
    }
    if (c->st == ST_WLCM_RD && (ev & NET_RD)){
        /* дочитать Welcome */
        while (c->in_got < sizeof(WelcomePkt)){
            tcp_iovec v = { c->in_buf + c->in_got, sizeof(WelcomePkt) - c->in_got };
            long rc = tcp_readv((tcp_fd_t)fd, &v, 1);
            if (rc > 0){ c->in_got += (size_t)rc; }
            else if (rc == 0){ cli_to_idle(c); return; }
            else { int err = net_last_error(); if (!net_err_would_block(err)){ cli_to_idle(c); } break; }
        }
        if (c->in_got >= sizeof(WelcomePkt)){
            const WelcomePkt* wp = (const WelcomePkt*)c->in_buf;
            if (wp->magic != HS_MAGIC_WLCM || wp->ver != HS_VER || wp->console_id != c->console_id){
                cli_to_idle(c); return;
            }
            /* перейти в STREAM (Cow1) */
            c->st = ST_STREAM;
            c->cow = cow1tcp_create(c->np, fd, on_op_from_server, c);
            /* flush буфер */
            flush_queue(c);
        }
    }
}

/* ===== VTable ===== */
static void cli_destroy(Replicator* rr){
    if (!rr) return;
    CliImpl* c = (CliImpl*)rr->impl;
    if (c){
        cli_to_idle(c);
        for (int i=0;i<c->qn;i++) free_op_payloads(&c->q[i]);
        free(c);
    }
    free(rr);
}

static void cli_publish(Replicator* rr, const ConOp* op){
    if (!rr || !op) return;
    CliImpl* c = (CliImpl*)rr->impl;
    if (c->st == ST_STREAM && c->cow){
        cow1tcp_send(c->cow, op);
    } else {
        queue_op(c, op);
    }
}

static void cli_set_listener(Replicator* rr, TopicId topic, ReplicatorConfirmCb cb, void* user){
    if (!rr || !cb) return;
    CliImpl* c = (CliImpl*)rr->impl;
    if (c->ln >= CLIENT_MAX_LISTENERS) return;
    c->ls[c->ln].cb = cb;
    c->ls[c->ln].user = user;
    c->ls[c->ln].cid = topic.inst_id; /* 0 — wildcard */
    c->ln++;
}

static void cli_unset_listener(Replicator* rr, TopicId topic){
    if (!rr) return;
    CliImpl* c = (CliImpl*)rr->impl;
    uint64_t cid = topic.inst_id;
    int w=0; for (int i=0;i<c->ln;i++){ if (!(c->ls[i].cid==cid || cid==0)) c->ls[w++] = c->ls[i]; }
    c->ln = w;
}

static int cli_caps(Replicator* rr){ (void)rr; return REPL_ORDERED | REPL_RELIABLE; }
static int cli_health(Replicator* rr){
    if (!rr) return -1;
    CliImpl* c = (CliImpl*)rr->impl;
    return (c && c->st==ST_STREAM && c->cow) ? 0 : -1;
}

static const ReplicatorVt CLI_VT = {
    .destroy      = cli_destroy,
    .publish      = cli_publish,
    .set_listener = cli_set_listener,
    .unset_listener = cli_unset_listener,
    .capabilities = cli_caps,
    .health       = cli_health,
};

/* ===== Публичные фабрики/API ===== */
Replicator* replicator_create_client_tcp(NetPoller* np, uint64_t console_id,
                                         const char* host, uint16_t port)
{
    CliImpl* impl = (CliImpl*)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
    impl->np = np;
    impl->console_id = console_id;
    impl->st = ST_IDLE;
    impl->fd = (net_fd_t)(intptr_t)-1;
    impl->ln = 0; impl->qn = 0;
    impl->host[0] = 0; impl->port = 0;
    if (host && *host){ strncpy(impl->host, host, sizeof(impl->host)-1); impl->port = port; }

    Replicator* r = (Replicator*)calloc(1, sizeof(*r));
    if (!r){ free(impl); return NULL; }
    r->v = &CLI_VT; r->impl = impl;
    /* если хост задан — сразу инициировать подключение */
    if (host && *host) repl_client_tcp_connect(r, host, port);
    return r;
}

int repl_client_tcp_connect(Replicator* rr, const char* host, uint16_t port){
    if (!rr) return -1;
    CliImpl* c = (CliImpl*)rr->impl; if (!c || !c->np) return -1;
    if (!host || !*host || !port) return -1;
    /* сбросить текущее состояние и стартовать новый connect */
    cli_to_idle(c);
    strncpy(c->host, host, sizeof(c->host)-1);
    c->port = port;
    tcp_fd_t sfd = (tcp_fd_t)TCP_INVALID_FD;
    int rc = tcp_connect(c->host, c->port, /*set_nb=*/1, &sfd);
    if (rc == NET_OK){
        /* сразу пишем HELO */
        c->fd = (net_fd_t)sfd;
        uint8_t out[sizeof(HelloPkt)];
        HelloPkt hp; hp.magic=HS_MAGIC_HELO; hp.ver=HS_VER; hp._rsv=0; hp.console_id=c->console_id;
        memcpy(out, &hp, sizeof(hp));
        memcpy(c->out_buf, out, sizeof(out));
        c->out_off=0; c->out_len=sizeof(out);
        c->st = ST_HELO_WR;
        net_poller_add(c->np, c->fd, NET_WR|NET_ERR, on_hs_cb, c);
        return 0;
    } else if (rc == NET_INPROGRESS){
        c->fd = (net_fd_t)sfd;
        c->st = ST_CONNECTING;
        net_poller_add(c->np, c->fd, NET_WR|NET_ERR, on_connect_cb, c);
        return 0;
    } else {
        return -1;
    }
}

void repl_client_tcp_disconnect(Replicator* rr){
    if (!rr) return;
    CliImpl* c = (CliImpl*)rr->impl;
    if (!c) return;
    cli_to_idle(c);
}

int repl_client_tcp_is_connected(Replicator* rr){
    if (!rr) return 0;
    CliImpl* c = (CliImpl*)rr->impl;
    return (c && c->st==ST_STREAM && c->cow) ? 1 : 0;
}
#endif /* !__EMSCRIPTEN__ */
