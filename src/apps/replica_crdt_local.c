#include "console/replicator.h"

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#endif

#include <errno.h>
#include "net/net.h"
#include "net/tcp.h"
#include "net/conop_wire.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>


/* ====== Локальный CRDT фан-аут + СЕТЕВОЙ ЛИДЕР  ======
   - Базово: локальная рассылка слушателям (как раньше).
   - Опционально: лидер TCP — принимает клиентов (HELLO/WELCOME) и шлёт/принимает ConOp. */

/* --- Параметры --- */
#ifndef REPL_SRV_MAX_CLIENTS
#  define REPL_SRV_MAX_CLIENTS 32
#endif
#ifndef REPL_SRV_ACCEPT_BUDGET
#  define REPL_SRV_ACCEPT_BUDGET 16
#endif

/* --- Handshake: fixed 16 bytes --- */
#pragma pack(push,1)
typedef struct {
    char     magic[4];      /* "HELO" / "WLCM" */
    uint16_t ver;           /* 1 */
    uint16_t reserved;      /* 0 */
    uint64_t console_id;    /* целевая консоль */
} ReplHelloPkt;
#pragma pack(pop)
#define REPL_HELLO_MAGIC  "HELO"
#define REPL_WELCOME_MAGIC "WLCM"
#define REPL_PROTO_VER  ((uint16_t)1)

typedef struct RepClient {
    net_fd_t    fd;
    int         state;      /* 0 = ждём HELLO, 1 = готов (COW1) */
    size_t      hs_got;     /* сколько байт HELLO уже получено */
    ReplHelloPkt hs;        /* приёмный буфер хендшейка */
    /* потоковый декодер COW1 (после хендшейка) */
    Cow1Decoder dec;
    /* исходящая очередь (кадры уже закодированы conop_wire_encode) */
    uint8_t*    out;
    size_t      out_len, out_cap, out_off;
    int         want_wr;
} RepClient;

typedef struct Listener {
    ReplicatorConfirmCb cb;
    void* user;
    ConsoleId cid; /* подписка на конкретную консоль; 0 = wildcard */
} Listener;


/* ==== Local backend: внутреннее состояние ==== */

typedef struct RepClient RepClient;          /* forward decl: структура клиента определяется ниже */

#ifndef REPL_MAX_LISTENERS
#  define REPL_MAX_LISTENERS 32
#endif
#ifndef REPL_SRV_MAX_CLIENTS
#  define REPL_SRV_MAX_CLIENTS 64
#endif

/* ===== Общая основа для всех репликаторов (внутренняя) ===== */
typedef struct ReplicatorVTable {
    void (*destroy)(void* self);
    void (*set_confirm)(void* self, ConsoleId console_id, ReplicatorConfirmCb cb, void* user);
    void (*publish)(void* self, const ConOp* op);
} ReplicatorVTable;

struct Replicator {
    const ReplicatorVTable* vt;
};

/* ==== Local backend: внутреннее состояние ==== */
#ifndef REPL_MAX_LISTENERS
#  define REPL_MAX_LISTENERS 32
#endif

typedef struct LocalRepl {
    struct Replicator base;   /* <-- первым! для upcast в Replicator* */
    NetPoller* np;
    ConsoleId  console_id;

    /* слушатели подтверждений */
    struct Listener ls[REPL_MAX_LISTENERS];
    int        n;

    /* активные клиенты сервера */
    RepClient* clients[REPL_SRV_MAX_CLIENTS];
    int        nclients;

    /* серверная часть (опционально) */
    tcp_fd_t   listen_fd;
    int        listening;
} LocalRepl;


/* --- реализация vtable для LocalRepl --- */
static void local_destroy(void* self){ local_repl_destroy((LocalRepl*)self); }

static void local_set_confirm(void* self, ConsoleId console_id,
                              ReplicatorConfirmCb cb, void* user){
    local_repl_set_confirm_listener((LocalRepl*)self, console_id, cb, user);
}

static void local_publish(void* self, const ConOp* op){
    local_repl_publish((LocalRepl*)self, op);
}

static const ReplicatorVTable LOCAL_VT = {
    .destroy     = local_destroy,
    .set_confirm = local_set_confirm,
    .publish     = local_publish,
};

/* ===== Общая основа для всех репликаторов (внутренняя) ===== */
typedef struct ReplicatorVTable {
    void (*destroy)(void* self);
    void (*set_confirm)(void* self, ConsoleId console_id, ReplicatorConfirmCb cb, void* user);
    void (*publish)(void* self, const ConOp* op);
} ReplicatorVTable;

struct Replicator {
    const ReplicatorVTable* vt;
};

/* прототипы, чтобы on_srv_conn2 не ругался на implicit declaration */
static void srv_drop_client(LocalRepl* r, RepClient* c);

/* ========== Утилиты очереди вывода ========== */
static int out_ensure_cap(RepClient* c, size_t need){
    if (c->out_cap >= need) return 1;
    size_t n = c->out_cap ? c->out_cap*2 : 8192;
    while (n < need) n *= 2;
    void* nb = realloc(c->out, n);
    if (!nb) return 0;
    c->out = (uint8_t*)nb; c->out_cap = n; return 1;
}
static int out_push(RepClient* c, const uint8_t* frame, size_t n){
    if (!out_ensure_cap(c, c->out_len + n)) return 0;
    memcpy(c->out + c->out_len, frame, n);
    c->out_len += n; c->want_wr = 1; return 1;
}

/* ========== Локальная доставка слушателям ========== */
static void fanout_publish(LocalRepl* r, const ConOp* op){
    if (!r || !op) return;
    for (int i=0;i<r->n;i++){
        if (r->ls[i].cb && (r->ls[i].cid==0 || r->ls[i].cid==op->console_id)){
            r->ls[i].cb(r->ls[i].user, op);
        }
    }
}

/* ========== Сетевой лидирующий сервер ========== */
static void srv_client_close(LocalRepl* r, RepClient* c){
    if (!r || !c) return;
    net_poller_del(r->np, c->fd);
    net_close_fd(c->fd);
    free(c->out);
    free(c);
}
static void srv_detach_client(LocalRepl* r, RepClient* c){
    if (!r || !c) return;
    for (int i=0;i<r->nclients;i++){
        if (r->clients[i] == c){
            for (int j=i+1;j<r->nclients;j++) r->clients[j-1]=r->clients[j];
            r->nclients--;
            break;
        }
    }
}
static void srv_drop_client(LocalRepl* r, RepClient* c){
    srv_detach_client(r, c);
    srv_client_close(r, c);
}

static void srv_broadcast_encoded(LocalRepl* r, const uint8_t* frame, size_t len){
    if (!r || !frame || !len) return;
    for (int i=0;i<r->nclients;i++){
        RepClient* c = r->clients[i];
        if (!c) continue;
        if (out_push(c, frame, len)){
            int mask = NET_RD | NET_ERR | (c->want_wr ? NET_WR : 0);
            net_poller_add(r->np, c->fd, mask, NULL, NULL); /* просто обновим интересы */
        }
    }
}

static void srv_broadcast_op(LocalRepl* r, const ConOp* op){
    if (!r || !op || r->nclients==0) return;
    uint8_t* buf=NULL; size_t len=0;
    if (conop_wire_encode(op, &buf, &len) != 0) return;
    srv_broadcast_encoded(r, buf, len);
    free(buf);
}

/* forward decls callbacks */
static void on_srv_listen(void* user, net_fd_t fd, int ev);

/* Приём готового ConOp от клиента: применить локально и разослать всем */
static void srv_on_client_op(LocalRepl* r, const ConOp* op,
                             const char* tag, const void* data, size_t dlen,
                             const void* init, size_t ilen)
{
    (void)tag; (void)dlen; (void)init; (void)ilen; (void)data; (void)dlen;
    if (!r || !op) return;
    /* Важно: локально подтверждаем синхронно и транслируем всем клиентам. */
    /* Скопируем payload'ы как в publish(), чтобы соблюсти контракт. */
    ConOp tmp = *op;
    void* copy_data = NULL;
    void* copy_init = NULL;
    if (op->data && op->size){ copy_data = malloc(op->size); if (copy_data){ memcpy(copy_data, op->data, op->size); tmp.data=copy_data; } }
    if (op->init_blob && op->init_size) {
        copy_init = malloc(op->init_size);
        if (copy_init) {
            memcpy(copy_init, op->init_blob, op->init_size);
            tmp.init_blob = copy_init;
            tmp.init_size = op->init_size;
        }
    }
    fanout_publish(r, &tmp);          /* локально */
    srv_broadcast_op(r, &tmp);        /* сеть */
    free(copy_data);
    free(copy_init);
}


/* ==== Реальная версия on_srv_conn с нормальным доступом к r через net_poller API ==== */
/* Для аккуратности — обернём user в пару {LocalRepl*, RepClient*}. */
typedef struct {
    LocalRepl* r;
    RepClient*  c;
} ConnUser;

static void on_srv_conn2(void* user, net_fd_t fd, int ev){
    ConnUser* cu = (ConnUser*)user;
    if (!cu || !cu->r || !cu->c) return;
    LocalRepl* r = cu->r;
    RepClient*  c = cu->c;
    (void)fd;
    if (ev & NET_ERR){ srv_drop_client(r, c); free(cu); return; }
    /* RD: либо докачиваем HELLO, либо читаем COW1-кадры */
    if (ev & NET_RD){
        for (;;){
            if (c->state == 0){
                /* добираем 16 байт HELLO */
                uint8_t* p = (uint8_t*)&c->hs;
                int need = (int)sizeof(ReplHelloPkt) - (int)c->hs_got;
                if (need <= 0) break;
#if defined(_WIN32)
                int rc = recv(c->fd, (char*)p + c->hs_got, need, 0);
#else
                int rc = (int)recv(c->fd, (char*)p + c->hs_got, (size_t)need, 0);
#endif
                if (rc > 0){
                    c->hs_got += (size_t)rc;
                    if (c->hs_got == sizeof(ReplHelloPkt)){
                        /* проверим */
                        if (memcmp(c->hs.magic, REPL_HELLO_MAGIC, 4)!=0 || c->hs.ver!=REPL_PROTO_VER || c->hs.console_id!=r->console_id){
                            srv_drop_client(r, c); free(cu); return;
                        }
                        /* ответим WELCOME */
                        ReplHelloPkt w = {0};
                        memcpy(w.magic, REPL_WELCOME_MAGIC, 4);
                        w.ver = REPL_PROTO_VER; w.reserved=0; w.console_id = r->console_id;
                        out_push(c, (const uint8_t*)&w, sizeof(w));
                        c->state = 1;
                        cow1_decoder_init(&c->dec);
                        /* включим WR при наличии очереди */
                        int mask = NET_RD | NET_ERR | (c->want_wr ? NET_WR : 0);
                        net_poller_add(r->np, c->fd, mask, on_srv_conn2, cu);
                        /* продолжаем цикл чтения — вдруг уже пришли данные */
                        continue;
                    }
                } else if (rc == 0){
                    srv_drop_client(r, c); free(cu); return;
                } else {
#if defined(_WIN32)
                    int err = WSAGetLastError();
#else
                    int err = errno;
#endif
                    if (net_err_would_block(err)) break;
                    srv_drop_client(r, c); free(cu); return;
                }
            } else {
                /* читаем COW1 поток в декодер */
                uint8_t tmp[4096];
#if defined(_WIN32)
                int rc = recv(c->fd, (char*)tmp, (int)sizeof(tmp), 0);
#else
                int rc = (int)recv(c->fd, (char*)tmp, sizeof(tmp), 0);
#endif
                if (rc > 0){
                    cow1_decoder_consume(&c->dec, tmp, (size_t)rc);
                    for (;;){
                        ConOp op; char* tag=NULL; void* data=NULL; size_t dlen=0; void* init=NULL; size_t ilen=0;
                        int k = cow1_decoder_take_next(&c->dec, &op, &tag, &data, &dlen, &init, &ilen);
                        if (k <= 0) break;
                        /* Применить и разослать */
                        srv_on_client_op(r, &op, tag, data, dlen, init, ilen);
                        conop_wire_free_decoded(tag, data, init);
                    }
                } else if (rc == 0){
                    srv_drop_client(r, c); free(cu); return;
                } else {
#if defined(_WIN32)
                    int err = WSAGetLastError();
#else
                    int err = errno;
#endif
                    if (net_err_would_block(err)) break;
                    srv_drop_client(r, c); free(cu); return;
                }
            }
            break; /* защитный вышиб — читаем порционно */
        }
    }
    if (ev & NET_WR){
        while (c->want_wr && c->out_off < c->out_len){
            size_t left = c->out_len - c->out_off;
            int to_send = (left > (size_t)INT32_MAX) ? INT32_MAX : (int)left;
#if defined(_WIN32)
            int rc = send(c->fd, (const char*)c->out + c->out_off, to_send, 0);
#else
            int rc = (int)send(c->fd, (const char*)c->out + c->out_off, (size_t)to_send, 0);
#endif
            if (rc > 0){
                c->out_off += (size_t)rc;
            } else {
#if defined(_WIN32)
                int err = WSAGetLastError();
#else
                int err = errno;
#endif
                if (net_err_would_block(err)) break;
                srv_drop_client(r, c); free(cu); return;
            }
        }
        if (c->out_off >= c->out_len){
            c->out_off = c->out_len = 0;
            c->want_wr = 0;
        }
    }
    int mask = NET_RD | NET_ERR | (c->want_wr ? NET_WR : 0);
    net_poller_add(r->np, c->fd, mask, on_srv_conn2, cu);
}

static void on_srv_listen(void* user, net_fd_t fd, int ev){
    LocalRepl* r = (LocalRepl*)user;
    if (!r) return;
    if (!(ev & NET_RD)) return;
    int budget = REPL_SRV_ACCEPT_BUDGET;
    while (budget-- > 0){
        tcp_fd_t cfd = (tcp_fd_t)TCP_INVALID_FD;
        int rc = tcp_accept((tcp_fd_t)fd, /*nonblock=*/1, &cfd, NULL, NULL);
        if (rc == 1) break;       /* очередь пуста */
        if (rc < 0) break;        /* ошибка */
        if (r->nclients >= REPL_SRV_MAX_CLIENTS){ tcp_close(cfd); continue; }
        RepClient* c = (RepClient*)calloc(1, sizeof(RepClient));
        c->fd = (net_fd_t)cfd; c->state=0; c->hs_got=0; c->out=NULL; c->out_len=c->out_cap=c->out_off=0; c->want_wr=0;
        r->clients[r->nclients++] = c;
        ConnUser* cu = (ConnUser*)calloc(1, sizeof(ConnUser));
        cu->r = r; cu->c = c;
        net_poller_add(r->np, c->fd, NET_RD|NET_ERR, on_srv_conn2, cu);
    }
}

/* ====== Конструкторы/деструктор и общий API ====== */
LocalRepl* local_repl_create_crdt_local(void){
    LocalRepl* r = (LocalRepl*)calloc(1, sizeof(LocalRepl));
    return r;
}

LocalRepl* local_repl_create_server_tcp(NetPoller* np, uint16_t port, ConsoleId console_id){
    LocalRepl* r = local_repl_create_crdt_local();
    if (!r) return NULL;
    r->np = np;
    r->console_id = console_id;
#ifdef __EMSCRIPTEN__
    (void)port;
    r->listening = 0;  /* в wasm сети нет — остаёмся локальными */
    return r;
#else
    if (!np){ return r; }
    tcp_fd_t s = tcp_listen(port, 64);
    if ((intptr_t)s < 0){
        /* не удалось — оставим только локальный путь */
        r->listening = 0;
        return r;
    }
    r->listen_fd = s; r->listening = 1;
    net_poller_add(np, (net_fd_t)s, NET_RD|NET_ERR, on_srv_listen, r);
    return r;
#endif
}

void local_repl_destroy(LocalRepl* r){
    if (!r) return;
    /* закрыть сеть, если была */
    if (r->listening){
        net_poller_del(r->np, (net_fd_t)r->listen_fd);
        tcp_close(r->listen_fd);
        r->listening = 0;
    }
    for (int i=0;i<r->nclients;i++){
        if (r->clients[i]){
            net_poller_del(r->np, r->clients[i]->fd);
            net_close_fd(r->clients[i]->fd);
            free(r->clients[i]->out);
            free(r->clients[i]);
        }
    }
    free(r);
}

void local_repl_set_confirm_listener(LocalRepl* r, ConsoleId console_id, ReplicatorConfirmCb cb, void* user){
    if (!r || !cb) return;
    if (r->n < REPL_MAX_LISTENERS){
        r->ls[r->n].cb = cb;
        r->ls[r->n].user = user;
        r->ls[r->n].cid  = console_id;
        r->n++;
    }
}

void local_repl_publish(LocalRepl* r, const ConOp* op){
    if (!r || !op) return;
    /* 1) Скопировать payload’ы под контракт */
    ConOp tmp = *op;
    void* copy_data = NULL;
    void* copy_init = NULL;
    if (op->data && op->size){
        copy_data = malloc(op->size);
        if (copy_data){ memcpy(copy_data, op->data, op->size); tmp.data = copy_data; }
    }
    if (op->init_blob && op->init_size){
        copy_init = malloc(op->init_size);
        if (copy_init){ memcpy(copy_init, op->init_blob, op->init_size); tmp.init_blob = copy_init; }
    }
    /* 2) Синхронное локальное подтверждение (важно даже без клиентов) */
    fanout_publish(r, &tmp);
    /* 3) Сетевая рассылка всем текущим клиентам (включая источник, если это был клиент) */
    srv_broadcast_op(r, &tmp);
    /* 4) Освободить копии */
    free(copy_data);
    free(copy_init);
}




/* ========= Публичный API (единые точки входа) ========= */
Replicator* replicator_create_crdt_local(void){
    return (Replicator*)local_repl_create_crdt_local();
}

Replicator* replicator_create_leader_tcp(NetPoller* np, ConsoleId console_id, uint16_t port){
    return (Replicator*)local_repl_create_server_tcp(np, port, console_id);
}

void replicator_destroy(Replicator* rr){
    if (!rr || !rr->vt || !rr->vt->destroy) return;
    rr->vt->destroy(rr);
}

void replicator_set_confirm_listener(Replicator* rr, ConsoleId cid, ReplicatorConfirmCb cb, void* user){
    if (!rr || !rr->vt || !rr->vt->set_confirm) return;
    rr->vt->set_confirm(rr, cid, cb, user);
}

void replicator_publish(Replicator* rr, const ConOp* op){
    if (!rr || !rr->vt || !rr->vt->publish) return;
    rr->vt->publish(rr, op);
}
