#include "console/replicator.h"

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netdb.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#endif

#include "net/net.h"
#include "net/conop_wire.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

/* ===== Внутренние базовые типы (такие же, как в replica_crdt_local.c) ===== */
typedef struct ReplicatorVTable {
    void (*destroy)(void* self);
    void (*set_confirm)(void* self, ConsoleId console_id, ReplicatorConfirmCb cb, void* user);
    void (*publish)(void* self, const ConOp* op);
} ReplicatorVTable;

struct Replicator {
    const ReplicatorVTable* vt;
};

#ifndef REPL_MAX_LISTENERS
#  define REPL_MAX_LISTENERS 32
#endif

struct Listener {
    ReplicatorConfirmCb cb;
    void* user;
    ConsoleId cid;
};

/* --- Handshake: фиксированные 16 байт (совместимо с сервером) --- */
#pragma pack(push,1)
typedef struct {
    char     magic[4];      /* "HELO" / "WLCM" */
    uint16_t ver;           /* 1 */
    uint16_t reserved;      /* 0 */
    uint64_t console_id;    /* консоль для подписки */
} ReplHelloPkt;
#pragma pack(pop)
#define REPL_HELLO_MAGIC   "HELO"
#define REPL_WELCOME_MAGIC "WLCM"
#define REPL_PROTO_VER  ((uint16_t)1)

/* ===== TCP client replicator ===== */
typedef struct TcpClientRepl {
    struct Replicator base; /* vt = CLIENT_VT */

    NetPoller* np;
    net_fd_t   fd;
    int        state;       /* 0=connecting, 1=wait WLCM, 2=stream, -1=closed */
    ConsoleId  console_id;

    /* listeners */
    struct Listener ls[REPL_MAX_LISTENERS];
    int n;

    /* входящий поток */
    Cow1Decoder dec;
    size_t hs_got; ReplHelloPkt hs;

    /* исходящая очередь */
    uint8_t* out;
    size_t   out_len, out_cap, out_off;
    int      want_wr;

    /* target */
    char*    host;
    uint16_t port;
} TcpClientRepl;

/* ===== utils ===== */
static int out_ensure_cap(TcpClientRepl* r, size_t need){
    if (r->out_cap >= need) return 1;
    size_t n = r->out_cap ? r->out_cap*2 : 8192;
    while (n < need) n *= 2;
    void* nb = realloc(r->out, n);
    if (!nb) return 0;
    r->out = (uint8_t*)nb; r->out_cap = n; return 1;
}
static int out_push(TcpClientRepl* r, const uint8_t* frame, size_t n){
    if (!out_ensure_cap(r, r->out_len + n)) return 0;
    memcpy(r->out + r->out_len, frame, n);
    r->out_len += n; r->want_wr = 1; return 1;
}

static void fanout_confirm(TcpClientRepl* r, const ConOp* op){
    if (!r || !op) return;
    for (int i=0;i<r->n;i++){
        if (r->ls[i].cb && (r->ls[i].cid==0 || r->ls[i].cid==op->console_id)){
            r->ls[i].cb(r->ls[i].user, op);
        }
    }
}

/* ===== net helpers ===== */
static int set_nonblock(net_fd_t fd){
#if defined(_WIN32)
    u_long nb = 1;
    return ioctlsocket(fd, FIONBIO, &nb)==0 ? 0 : -1;
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

static int connect_nb(const char* host, uint16_t port, net_fd_t* out_fd, int* immediate_ok){
    if (!host || !out_fd || !immediate_ok) return -1;
    char svc[16]; snprintf(svc, sizeof(svc), "%u", (unsigned)port);

    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family   = AF_UNSPEC;

    struct addrinfo* ai = NULL;
    int gai = getaddrinfo(host, svc, &hints, &ai);
    if (gai != 0) return -1;

    int ok = -1;
    for (struct addrinfo* p = ai; p; p = p->ai_next){
        net_fd_t s = (net_fd_t)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
#if defined(_WIN32)
        if ((intptr_t)s == (intptr_t)INVALID_SOCKET) continue;
#else
        if ((intptr_t)s < 0) continue;
#endif
        if (set_nonblock(s) != 0){
#if defined(_WIN32)
            closesocket(s);
#else
            close(s);
#endif
            continue;
        }
        int rc = connect(s, p->ai_addr, (socklen_t)p->ai_addrlen);
        if (rc == 0){
            *out_fd = s; *immediate_ok = 1; ok = 0; break;
        }
#if defined(_WIN32)
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS){
            *out_fd = s; *immediate_ok = 0; ok = 0; break;
        }
        closesocket(s);
#else
        if (errno == EINPROGRESS || errno == EWOULDBLOCK){
            *out_fd = s; *immediate_ok = 0; ok = 0; break;
        }
        close(s);
#endif
    }
    freeaddrinfo(ai);
    return ok;
}

static void client_close(TcpClientRepl* r){
    if (!r) return;
    if ((intptr_t)r->fd >= 0){
        net_poller_del(r->np, r->fd);
        net_close_fd(r->fd);
        r->fd = (net_fd_t)-1;
    }
    r->state = -1;
    r->want_wr = 0;
    r->out_off = r->out_len = 0;
}

/* ===== основной обработчик poller'а ===== */
static void on_cli(void* user, net_fd_t fd, int ev){
    TcpClientRepl* r = (TcpClientRepl*)user;
    if (!r || fd != r->fd) return;

    /* завершение connect(): ждём WR или ERR */
    if (r->state == 0 && (ev & (NET_WR|NET_ERR))){
        int soerr = 0; socklen_t sl = (socklen_t)sizeof(soerr);
        if (getsockopt(r->fd, SOL_SOCKET, SO_ERROR, (void*)&soerr, &sl) != 0 || soerr != 0){
            client_close(r); return;
        }
        /* connected — шлём HELLO */
        ReplHelloPkt h; memset(&h, 0, sizeof(h));
        memcpy(h.magic, REPL_HELLO_MAGIC, 4);
        h.ver = REPL_PROTO_VER; h.reserved=0; h.console_id=r->console_id;
        (void)out_push(r, (const uint8_t*)&h, sizeof(h));
        r->state = 1; r->hs_got = 0;
    }

    /* чтение */
    if (ev & NET_RD){
        for (;;){
            if (r->state == 1){
                /* читаем 16 байт WLCM */
                uint8_t* p = (uint8_t*)&r->hs;
                int need = (int)sizeof(ReplHelloPkt) - (int)r->hs_got;
                if (need > 0){
#if defined(_WIN32)
                    int rc = recv(r->fd, (char*)p + r->hs_got, need, 0);
#else
                    int rc = (int)recv(r->fd, (char*)p + r->hs_got, (size_t)need, 0);
#endif
                    if (rc > 0){
                        r->hs_got += (size_t)rc;
                        if (r->hs_got == sizeof(ReplHelloPkt)){
                            if (memcmp(r->hs.magic, REPL_WELCOME_MAGIC, 4)!=0 ||
                                r->hs.ver!=REPL_PROTO_VER ||
                                r->hs.console_id!=r->console_id){
                                client_close(r); return;
                            }
                            cow1_decoder_init(&r->dec);
                            r->state = 2;
                        } else {
                            break; /* дочитываем в следующий раз */
                        }
                    } else if (rc == 0){
                        client_close(r); return;
                    } else {
#if defined(_WIN32)
                        int err = WSAGetLastError();
#else
                        int err = errno;
#endif
                        if (net_err_would_block(err)) break;
                        client_close(r); return;
                    }
                } else break;
            } else if (r->state == 2){
                uint8_t tmp[4096];
#if defined(_WIN32)
                int rc = recv(r->fd, (char*)tmp, (int)sizeof(tmp), 0);
#else
                int rc = (int)recv(r->fd, (char*)tmp, sizeof(tmp), 0);
#endif
                if (rc > 0){
                    cow1_decoder_consume(&r->dec, tmp, (size_t)rc);
                    for(;;){
                        ConOp op; char* tag=NULL; void* data=NULL; size_t dlen=0; void* init=NULL; size_t ilen=0;
                        int k = cow1_decoder_take_next(&r->dec, &op, &tag, &data, &dlen, &init, &ilen);
                        if (k <= 0) break;
                        /* скопировать payload'ы под контракт, сделать fanout */
                        ConOp tmpop = *op.self? &op : op; /* на случай, если ConOp не POD — безопасно просто копировать */
                        (void)tmpop; /* подавим предупреждение, если макросы */
                        ConOp cpy = op;
                        void* copy_data=NULL; void* copy_init=NULL;
                        if (op.data && op.size){ copy_data = malloc(op.size); if (copy_data){ memcpy(copy_data, op.data, op.size); cpy.data=copy_data; } }
                        if (op.init_blob && op.init_size){ copy_init = malloc(op.init_size); if (copy_init){ memcpy(copy_init, op.init_blob, op.init_size); cpy.init_blob=copy_init; cpy.init_size=op.init_size; } }
                        fanout_confirm(r, &cpy);
                        free(copy_data); free(copy_init);
                        conop_wire_free_decoded(tag, data, init);
                    }
                } else if (rc == 0){
                    client_close(r); return;
                } else {
#if defined(_WIN32)
                    int err = WSAGetLastError();
#else
                    int err = errno;
#endif
                    if (net_err_would_block(err)) break;
                    client_close(r); return;
                }
            } else {
                break;
            }
            break; /* порционно */
        }
    }

    /* запись */
    if (ev & NET_WR){
        while (r->want_wr && r->out_off < r->out_len){
            size_t left = r->out_len - r->out_off;
            int to_send = (left > (size_t)INT32_MAX) ? INT32_MAX : (int)left;
#if defined(_WIN32)
            int rc = send(r->fd, (const char*)r->out + r->out_off, to_send, 0);
#else
            int rc = (int)send(r->fd, (const char*)r->out + r->out_off, (size_t)to_send, 0);
#endif
            if (rc > 0){
                r->out_off += (size_t)rc;
            } else {
#if defined(_WIN32)
                int err = WSAGetLastError();
#else
                int err = errno;
#endif
                if (net_err_would_block(err)) break;
                client_close(r); return;
            }
        }
        if (r->out_off >= r->out_len){
            r->out_off = r->out_len = 0;
            r->want_wr = 0;
        }
    }

    /* обновим интересы */
    if ((intptr_t)r->fd >= 0){
        int mask = NET_ERR | NET_RD | (r->want_wr ? NET_WR : 0);
        if (r->state == 0) mask = NET_ERR | NET_WR; /* ждём завершения connect */
        net_poller_add(r->np, r->fd, mask, on_cli, r);
    }
}

/* ===== vtable ===== */
static void client_destroy(void* self){
    TcpClientRepl* r = (TcpClientRepl*)self;
    if (!r) return;
    client_close(r);
    free(r->out);
    free(r->host);
    free(r);
}
static void client_set_confirm(void* self, ConsoleId cid, ReplicatorConfirmCb cb, void* user){
    TcpClientRepl* r = (TcpClientRepl*)self; if (!r || !cb) return;
    if (r->n < REPL_MAX_LISTENERS){
        r->ls[r->n].cb = cb;
        r->ls[r->n].user = user;
        r->ls[r->n].cid = cid;
        r->n++;
    }
}
static void client_publish(void* self, const ConOp* op){
    TcpClientRepl* r = (TcpClientRepl*)self; if (!r || !op) return;
    /* 1) локально подтверждаем сразу (идемпотентность по op_id на стороне приёмников) */
    ConOp tmp = *op;
    void* copy_data=NULL; void* copy_init=NULL;
    if (op->data && op->size){ copy_data = malloc(op->size); if (copy_data){ memcpy(copy_data, op->data, op->size); tmp.data=copy_data; } }
    if (op->init_blob && op->init_size){ copy_init = malloc(op->init_size); if (copy_init){ memcpy(copy_init, op->init_blob, op->init_size); tmp.init_blob=copy_init; tmp.init_size=op->init_size; } }
    fanout_confirm(r, &tmp);
    free(copy_data); free(copy_init);

    /* 2) отсылаем лидеру */
    if ((intptr_t)r->fd >= 0 && r->state >= 0){
        uint8_t* buf=NULL; size_t len=0;
        if (conop_wire_encode(op, &buf, &len) == 0 && buf && len){
            (void)out_push(r, buf, len);
        }
        free(buf);
        if ((intptr_t)r->fd >= 0){
            int mask = NET_ERR | NET_RD | NET_WR;
            if (r->state == 0) mask = NET_ERR | NET_WR;
            net_poller_add(r->np, r->fd, mask, on_cli, r);
        }
    }
}

static const ReplicatorVTable CLIENT_VT = {
    .destroy     = client_destroy,
    .set_confirm = client_set_confirm,
    .publish     = client_publish,
};

/* ===== public ctor ===== */
Replicator* replicator_create_client_tcp(NetPoller* np,
                                         const char* host,
                                         uint16_t port,
                                         ConsoleId console_id)
{
    if (!host){
        /* без host — лучше не рисковать, заведём локальный */
        extern Replicator* replicator_create_crdt_local(void);
        return replicator_create_crdt_local();
    }
    TcpClientRepl* r = (TcpClientRepl*)calloc(1, sizeof(TcpClientRepl));
    if (!r) return NULL;
    r->base.vt = &CLIENT_VT;
    r->np = np;
    r->console_id = console_id;
    r->fd = (net_fd_t)-1;
    r->state = 0;
    r->host = strdup(host);
    r->port = port;

    if (!np){
        /* без поллера — работаем чисто локально */
        return (Replicator*)r;
    }

    int immediate_ok = 0;
    net_fd_t s = (net_fd_t)-1;
    if (connect_nb(host, port, &s, &immediate_ok) != 0){
        /* fallback: локальный */
        free(r->host); free(r);
        extern Replicator* replicator_create_crdt_local(void);
        return replicator_create_crdt_local();
    }
    r->fd = s;
    /* если подключились сразу — положим HELLO в очередь уже сейчас */
    if (immediate_ok){
        ReplHelloPkt h; memset(&h, 0, sizeof(h));
        memcpy(h.magic, REPL_HELLO_MAGIC, 4);
        h.ver = REPL_PROTO_VER; h.reserved=0; h.console_id=r->console_id;
        (void)out_push(r, (const uint8_t*)&h, sizeof(h));
        r->state = 1;
    } else {
        r->state = 0; /* ждём завершения connect по WR */
    }
    int mask = (r->state==0) ? (NET_ERR|NET_WR) : (NET_ERR|NET_RD|NET_WR);
    net_poller_add(np, r->fd, mask, on_cli, r);
    return (Replicator*)r;
}
