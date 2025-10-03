/* Репликатор-лидер по TCP: слушает, делает HELO/WLCM, ретранслирует ConOp всем клиентам.
 * Реализация бэкенда под общий интерфейс ReplicatorVt.
 */
#include "replication/backends/leader_tcp.h"
#include "replication/repl_iface.h"
#include "replication/repl_types.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/wire_tcp.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#ifndef REPL_SRV_MAX_CLIENTS
#  define REPL_SRV_MAX_CLIENTS 32
#endif
#ifndef REPL_SRV_ACCEPT_BUDGET
#  define REPL_SRV_ACCEPT_BUDGET 16
#endif
#ifndef REPL_MAX_LISTENERS
#  define REPL_MAX_LISTENERS 16
#endif

/* ===== Little-endian helpers ===== */
static inline void wr16(uint8_t** p, uint16_t v){ (*p)[0]=(uint8_t)(v); (*p)[1]=(uint8_t)(v>>8); *p+=2; }
static inline void wr32(uint8_t** p, uint32_t v){ (*p)[0]=(uint8_t)(v); (*p)[1]=(uint8_t)(v>>8); (*p)[2]=(uint8_t)(v>>16); (*p)[3]=(uint8_t)(v>>24); *p+=4; }
static inline void wr64(uint8_t** p, uint64_t v){ wr32(p,(uint32_t)(v&0xFFFFFFFFull)); wr32(p,(uint32_t)(v>>32)); }
static inline uint32_t rd32(const uint8_t** p){ const uint8_t* s=*p; *p+=4; return (uint32_t)s[0]|((uint32_t)s[1]<<8)|((uint32_t)s[2]<<16)|((uint32_t)s[3]<<24); }
static inline uint16_t rd16(const uint8_t** p){ const uint8_t* s=*p; *p+=2; return (uint16_t)(s[0]|((uint16_t)s[1]<<8)); }
static inline uint64_t rd64(const uint8_t** p){ uint64_t lo=rd32(p), hi=rd32(p); return lo|(hi<<32); }

/* ===== HELO/WLCM ===== */
#define HS_MAGIC_HELO 0x4F4C4548u /* 'HELO' LE */
#define HS_MAGIC_WLCM 0x4D434C57u /* 'WLCM' LE */
#define HS_VER        1u
typedef struct {
    uint32_t magic;   /* HELO */
    uint16_t ver;     /* 1 */
    uint16_t _rsv;
    uint64_t console_id;
} HelloPkt;                   /* 16 bytes */
typedef HelloPkt WelcomePkt;  /* те же поля, magic=WLCM */

typedef struct Listener {
    ReplicatorConfirmCb cb;
    void* user;
    uint64_t cid; /* 0 = wildcard, иначе фильтр по op->console_id */
} Listener;

struct LeaderRepl; /* fwd */

typedef struct Client {
    net_fd_t fd;
    int      state;   /* 0=ожидаем HELO; 1=пишем WLCM; 2=stream(COW1) */
    size_t   hs_got;
    uint8_t  hs_buf[sizeof(HelloPkt)];
    size_t   out_off, out_len;
    uint8_t  out_buf[sizeof(WelcomePkt)];
    Cow1Tcp* cow;     /* живёт в состоянии 2 */
    struct LeaderRepl* owner;
} Client;

typedef struct LeaderRepl {
    NetPoller* np;
    tcp_fd_t   listen_fd;
    uint64_t   console_id;
    Listener   ls[REPL_MAX_LISTENERS];
    int        ln;
    Client     cl[REPL_SRV_MAX_CLIENTS];
    int        cn;      /* занятых клиентов */
} LeaderRepl;

/* ===== локальная доставка подтверждений ===== */
static void fanout_local(LeaderRepl* r, const ConOp* op){
    if (!r||!op) return;
    for (int i=0;i<r->ln;i++){
        if (r->ls[i].cb && (r->ls[i].cid==0 || r->ls[i].cid==op->console_id)){
            r->ls[i].cb(r->ls[i].user, op);
        }
    }
}

static void client_close(LeaderRepl* r, int idx){
    if (!r || idx<0 || idx>=r->cn) return;
    Client* c = &r->cl[idx];
    if (c->cow){ cow1tcp_destroy(c->cow); c->cow=NULL; }
    if ((intptr_t)c->fd >= 0){ net_poller_del(r->np, c->fd); tcp_close((tcp_fd_t)c->fd); }
    /* compact */
    for (int j=idx+1;j<r->cn;j++) r->cl[j-1] = r->cl[j];
    r->cn--;
}

/* ===== колбэк из COW1 — пришёл ConOp от клиента ===== */
static void srv_on_client_op(void* user,
                             const ConOp* inop,
                             const char* tag,
                             const void* data, size_t dlen,
                             const void* init, size_t ilen)
{
    (void)tag;
    Client* c = (Client*)user; if (!c || !c->owner || !inop) return;
    LeaderRepl* r = c->owner;

    /* Скопировать payload'ы — cow1tcp освободит свои буферы после колбэка */
    ConOp op = *inop;
    void* copy_data = NULL; void* copy_init = NULL;
    if (data && dlen){ copy_data = malloc(dlen); if (copy_data){ memcpy(copy_data, data, dlen); op.data = copy_data; op.size = dlen; } }
    if (init && ilen){ copy_init = malloc(ilen); if (copy_init){ memcpy(copy_init, init, ilen); op.init_blob = copy_init; op.init_size = ilen; } }

    /* 1) локально подтвердить */
    fanout_local(r, &op);
    /* 2) ретрансмит всем, включая отправителя */
    for (int i=0;i<r->cn;i++){
        if (r->cl[i].cow){ cow1tcp_send(r->cl[i].cow, &op); }
    }
    free(copy_data);
    free(copy_init);
}

/* ===== Handshake HELO/WLCM и переход в COW1 ===== */
static void on_client_hs(void* user, net_fd_t fd, int ev){
    LeaderRepl* r = (LeaderRepl*)user; if (!r) return;
    /* найти клиента по fd */
    int idx = -1;
    for (int i=0;i<r->cn;i++){ if (r->cl[i].fd == fd){ idx = i; break; } }
    if (idx < 0) return;
    Client* c = &r->cl[idx];
    if (ev & NET_ERR){ client_close(r, idx); return; }
    if (c->state == 0 && (ev & NET_RD)){
        /* дочитываем HELO */
        size_t need = sizeof(HelloPkt) - c->hs_got;
        if (need){
            tcp_iovec v = { c->hs_buf + c->hs_got, need };
            long rc = tcp_readv((tcp_fd_t)c->fd, &v, 1);
            if (rc > 0){ c->hs_got += (size_t)rc; }
            else if (rc == 0){ client_close(r, idx); return; }
            else { /* rc<0 */ int err = net_last_error(); if (!net_err_would_block(err)){ client_close(r, idx); } return; }
        }
        if (c->hs_got >= sizeof(HelloPkt)){
            /* проверить и подготовить WLCM */
            const uint8_t* p = c->hs_buf;
            uint32_t magic = rd32(&p);
            uint16_t ver   = rd16(&p); (void)rd16(&p);
            uint64_t cid   = rd64(&p);
            if (magic != HS_MAGIC_HELO || ver != HS_VER || cid != r->console_id){
                client_close(r, idx); return;
            }
            /* сформировать WLCM для записи */
            uint8_t out[sizeof(WelcomePkt)];
            uint8_t* q = out;
            wr32(&q, HS_MAGIC_WLCM); wr16(&q, HS_VER); wr16(&q, 0); wr64(&q, r->console_id);
            memcpy(c->out_buf, out, sizeof(out));
            c->out_off = 0; c->out_len = sizeof(out);
            c->state = 1;
            net_poller_mod(r->np, c->fd, NET_RD|NET_WR|NET_ERR);
        }
    }
    if (c->state == 1 && (ev & NET_WR)){
        /* писать WLCM (может уйти частично) */
        size_t left = c->out_len - c->out_off;
        if (left){
            size_t wrote = 0;
            int rc = tcp_write_all((tcp_fd_t)c->fd, c->out_buf + c->out_off, left, &wrote);
            c->out_off += wrote;
            if (rc < 0){ client_close(r, idx); return; }
            /* rc==1 — would block; rc==0 — всё ушло */
        }
        if (c->out_off >= c->out_len){
            /* перейти в поток COW1 */
            c->state = 2;
            c->out_off = c->out_len = 0;
            /* заменить обработчик на Cow1Tcp */
            c->cow = cow1tcp_create(r->np, c->fd, srv_on_client_op, c);
            /* cow1tcp сам модифицирует интересы fd в поллере */
        }
    }
}

static void on_listen(void* user, net_fd_t fd, int ev){
    LeaderRepl* r = (LeaderRepl*)user; if (!r) return;
    if (!(ev & NET_RD)) return;
    int budget = REPL_SRV_ACCEPT_BUDGET;
    while (budget-- > 0){
        tcp_fd_t cfd = (tcp_fd_t)TCP_INVALID_FD;
        int rc = tcp_accept((tcp_fd_t)fd, /*nonblock=*/1, &cfd, NULL, NULL);
        if (rc == 1) break;      /* очередь пуста */
        if (rc < 0) break;       /* ошибка */
        if (r->cn >= REPL_SRV_MAX_CLIENTS){ tcp_close(cfd); continue; }
        Client* c = &r->cl[r->cn++];
        memset(c, 0, sizeof(*c));
        c->owner = r;
        c->fd = (net_fd_t)cfd;
        c->state = 0; c->hs_got = 0;
        /* подписываемся на RD/ERR — руками ведём handshake, затем переключим на Cow1Tcp */
        net_poller_add(r->np, c->fd, NET_RD|NET_ERR, on_client_hs, r);
    }
}

/* ===== VTable реализация ===== */
static void leader_destroy(Replicator* rr){
    if (!rr) return;
    LeaderRepl* r = (LeaderRepl*)rr->impl;
    if (r){
        /* клиенты */
        for (int i=0;i<r->cn;i++){
            if (r->cl[i].cow){ cow1tcp_destroy(r->cl[i].cow); r->cl[i].cow=NULL; }
            if ((intptr_t)r->cl[i].fd >= 0){
                net_poller_del(r->np, r->cl[i].fd);
                tcp_close((tcp_fd_t)r->cl[i].fd);
            }
        }
        r->cn = 0;
        /* слушатель */
        if ((intptr_t)r->listen_fd >= 0){
            net_poller_del(r->np, (net_fd_t)r->listen_fd);
            tcp_close(r->listen_fd);
            r->listen_fd = (tcp_fd_t)TCP_INVALID_FD;
        }
        free(r);
    }
    free(rr);
}

static void leader_publish(Replicator* rr, const ConOp* op){
    if (!rr || !op) return;
    LeaderRepl* r = (LeaderRepl*)rr->impl;
    /* скопировать payload'ы, чтобы можно было асинхронно вещать */
    ConOp tmp = *op;
    void* copy_data = NULL; void* copy_init = NULL;
    if (op->data && op->size){ copy_data = malloc(op->size); if (copy_data){ memcpy(copy_data, op->data, op->size); tmp.data = copy_data; tmp.size = op->size; } }
    if (op->init_blob && op->init_size){ copy_init = malloc(op->init_size); if (copy_init){ memcpy(copy_init, op->init_blob, op->init_size); tmp.init_blob = copy_init; tmp.init_size = op->init_size; } }
    /* 1) локальное подтверждение сразу */
    fanout_local(r, &tmp);
    /* 2) рассылка всем клиентам */
    for (int i=0;i<r->cn;i++){
        if (r->cl[i].cow){ cow1tcp_send(r->cl[i].cow, &tmp); }
    }
    free(copy_data);
    free(copy_init);
}

static void leader_set_listener(Replicator* rr, TopicId topic, ReplicatorConfirmCb cb, void* user){
    if (!rr || !cb) return;
    LeaderRepl* r = (LeaderRepl*)rr->impl;
    if (r->ln < REPL_MAX_LISTENERS){
        r->ls[r->ln].cb = cb;
        r->ls[r->ln].user = user;
        /* для совместимости: фильтруем по console_id полю операции; используем topic.inst_id */
        r->ls[r->ln].cid = topic.inst_id; /* 0 — wildcard */
        r->ln++;
    }
}

static void leader_unset_listener(Replicator* rr, TopicId topic){
    if (!rr) return;
    LeaderRepl* r = (LeaderRepl*)rr->impl;
    uint64_t cid = topic.inst_id;
    int w=0;
    for (int i=0;i<r->ln;i++){
        if (!(r->ls[i].cid==cid || cid==0)) r->ls[w++] = r->ls[i];
    }
    r->ln = w;
}

static int leader_capabilities(Replicator* rr){
    (void)rr;
    return REPL_ORDERED | REPL_RELIABLE | REPL_BROADCAST;
}

static int leader_health(Replicator* rr){
    if (!rr) return -1;
    LeaderRepl* r = (LeaderRepl*)rr->impl;
    if (!r) return -1;
    return ((intptr_t)r->listen_fd >= 0) ? 0 : -1;
}

static const ReplicatorVt LEADER_VT = {
    .destroy      = leader_destroy,
    .publish      = leader_publish,
    .set_listener = leader_set_listener,
    .unset_listener = leader_unset_listener,
    .capabilities = leader_capabilities,
    .health       = leader_health,
};

/* ===== Фабрика ===== */
Replicator* replicator_create_leader_tcp(NetPoller* np, uint64_t console_id, uint16_t port){
    if (!np) return NULL;
    LeaderRepl* impl = (LeaderRepl*)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
    impl->np = np;
    impl->console_id = console_id;
    impl->listen_fd = tcp_listen(port, 64);
    if ((intptr_t)impl->listen_fd < 0){
        free(impl);
        return NULL;
    }
    net_poller_add(np, (net_fd_t)impl->listen_fd, NET_RD|NET_ERR, on_listen, impl);

    Replicator* r = (Replicator*)calloc(1, sizeof(*r));
    if (!r){
        net_poller_del(np, (net_fd_t)impl->listen_fd);
        tcp_close(impl->listen_fd);
        free(impl);
        return NULL;
    }
    r->v    = &LEADER_VT;
    r->impl = impl;
    return r;
}
