/* CRDT mesh backend — TCP mesh с дедупом и снапшотом при подключении. */
#include "replication/backends/crdt_mesh.h"
#include "replication/repl_iface.h"
#include "replication/repl_types.h"
#include "replication/type_registry.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/wire_tcp.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef __EMSCRIPTEN__

/* === Настройки по умолчанию === */
#ifndef CRDT_MAX_LISTENERS
#  define CRDT_MAX_LISTENERS 16
#endif
#ifndef CRDT_MAX_PEERS
#  define CRDT_MAX_PEERS 64
#endif
#ifndef CRDT_MAX_TOPICS
#  define CRDT_MAX_TOPICS 64
#endif
#ifndef CRDT_DEDUP_CAP
#  define CRDT_DEDUP_CAP 8192 /* пар (console_id,op_id) */
#endif

typedef struct Listener {
    ReplicatorConfirmCb cb;
    void* user;
    uint64_t inst_id; /* фильтр по op->console_id (0 = wildcard) */
} Listener;


typedef struct TopicRec {
    TopicId t;
    int have;
} TopicRec;


typedef struct Peer {
    net_fd_t  fd;
    Cow1Tcp*  cow;
    int       alive;
    struct CrdtMesh* owner;
} Peer;

typedef struct DedupEnt {
    uint64_t console_id;
    uint64_t op_id;
} DedupEnt;


typedef struct CrdtMesh {
    NetPoller* np;
    tcp_fd_t   listen_fd; /* <0, если не слушаем */
    uint16_t   port;      /* порт mesh (для seed add) */
    Listener  ls[CRDT_MAX_LISTENERS]; int ln;
    TopicRec  topics[CRDT_MAX_TOPICS]; int tn;
    Peer      peers[CRDT_MAX_PEERS];   int pn;
    /* Простая хеш-таблица для дедупликации операций. */
    DedupEnt*  dedup;
    size_t     dcap;
    size_t     dcount;
} CrdtMesh;

/* ============ малые утилиты ============ */
static void fanout_local(CrdtMesh* r, const ConOp* op){
    if (!r || !op) return;
    for (int i=0;i<r->ln;i++){
        Listener* L = &r->ls[i];
        if (L->cb && (L->inst_id==0 || L->inst_id==op->console_id)){
            L->cb(L->user, op);
        }
    }
}

static int topic_eq(TopicId a, TopicId b){ return a.type_id==b.type_id && a.inst_id==b.inst_id; }
static int topic_have(CrdtMesh* r, TopicId t){
    for (int i=0;i<r->tn;i++) if (r->topics[i].have && topic_eq(r->topics[i].t, t)) return 1;
    return 0;
}

/* ===== Дедуп по (console_id, op_id) — открытая адресация ===== */
static uint64_t mix64(uint64_t x){ /* xorshift* */
    x ^= x>>30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x>>27; x *= 0x94d049bb133111ebULL;
    x ^= x>>31; return x;
}
static size_t d_hash(uint64_t a, uint64_t b, size_t cap){
    return (size_t)(mix64(a*0x9E3779B185EBCA87ULL ^ b) % (uint64_t)cap);
}
static int d_probe_find(CrdtMesh* r, uint64_t console_id, uint64_t op_id, size_t* out_idx){
    if (!r->dedup || r->dcap==0){ if(out_idx)*out_idx=0; return 0; }
    size_t i = d_hash(console_id, op_id, r->dcap);
    for (size_t step=0; step<r->dcap; ++step){
        size_t k = (i + step) % r->dcap;
        DedupEnt* e = &r->dedup[k];
        if (e->console_id==0 && e->op_id==0){ if(out_idx)*out_idx=k; return 0; } /* empty */
        if (e->console_id==console_id && e->op_id==op_id){ if(out_idx)*out_idx=k; return 1; } /* found */
    }
    if (out_idx)*out_idx=0;
    return 0;
}
static int d_grow(CrdtMesh* r, size_t ncap){
    DedupEnt* nb = (DedupEnt*)calloc(ncap, sizeof(DedupEnt));
    if (!nb) return 0;
    DedupEnt* ob = r->dedup; size_t oc = r->dcap;
    r->dedup = nb; r->dcap = ncap; r->dcount = 0;
    if (ob){
        for (size_t i=0;i<oc;i++){
            if (ob[i].console_id||ob[i].op_id){
                size_t idx=0; /* никогда не занят */
                /* reinsert */
                size_t h = d_hash(ob[i].console_id, ob[i].op_id, r->dcap);
                for (size_t st=0;st<r->dcap;st++){
                    size_t k=(h+st)%r->dcap;
                    if (r->dedup[k].console_id==0 && r->dedup[k].op_id==0){ idx=k; break; }
                }
                r->dedup[idx] = ob[i];
                r->dcount++;
            }
        }
        free(ob);
    }
    return 1;
}
static int d_insert(CrdtMesh* r, uint64_t console_id, uint64_t op_id){
    if (!r->dedup){ if(!d_grow(r, CRDT_DEDUP_CAP)) return 0; }
    if (r->dcount*2 >= r->dcap){ if(!d_grow(r, r->dcap ? r->dcap*2 : CRDT_DEDUP_CAP)) return 0; }
    size_t idx=0;
    if (d_probe_find(r, console_id, op_id, &idx)) return 1; /* уже есть */
    r->dedup[idx].console_id = console_id;
    r->dedup[idx].op_id      = op_id;
    r->dcount++;
    return 1;
}
static int d_seen(CrdtMesh* r, uint64_t console_id, uint64_t op_id){
    size_t idx=0;
    return d_probe_find(r, console_id, op_id, &idx);
}

/* ===== Отправка снапшота всем пирами ===== */
static void send_snapshot_to_peer(CrdtMesh* r, Peer* p, TopicId t){
    if (!r || !p || !p->cow) return;
    void* user=NULL;
    const TypeVt* vt = type_registry_get_default(t.type_id, &user);
    if (!vt || !vt->snapshot) return;
    void* blob=NULL; size_t blen=0; uint32_t schema=0;
    if (vt->snapshot(user, &schema, &blob, &blen) == 0 && blob && blen>0){
        ConOp sop = (ConOp){0};
        sop.topic = t;
        sop.console_id = t.inst_id;
        sop.schema = schema;        // <--- теперь несём версию
        sop.tag = "snapshot";
        sop.init_blob = blob; sop.init_size = blen;
        (void)cow1tcp_send(p->cow, &sop);
        free(blob);
    }
}

static void send_snapshots_to_peer(CrdtMesh* r, Peer* p){
    for (int i=0;i<r->tn;i++){
        if (r->topics[i].have) send_snapshot_to_peer(r, p, r->topics[i].t);
    }
}

/* ===== Управление пирами ===== */
static void peer_destroy(CrdtMesh* r, int idx){
    if (!r || idx<0 || idx>=r->pn) return;
    Peer* p = &r->peers[idx];
    if (p->cow){ cow1tcp_destroy(p->cow); p->cow=NULL; }
    if ((intptr_t)p->fd >= 0){ net_poller_del(r->np, p->fd); tcp_close((tcp_fd_t)p->fd); }
    /* compact */
    for (int j=idx+1;j<r->pn;j++) r->peers[j-1] = r->peers[j];
    r->pn--;
}

/* on_op callback из Cow1: получен ConOp от пира */
static void on_peer_op(void* user,
                       const ConOp* inop,
                       const char* tag,
                       const void* data, size_t dlen,
                       const void* init, size_t ilen)
{
    (void)tag;
    Peer* from = (Peer*)user;
    CrdtMesh* r = from ? from->owner : NULL;
    if (!r) return;

    /* Дедупликация только для «обычных» операций; снапшоты (init_blob && op_id==0)
       пропускаем как есть. */
    if (!(inop->init_blob && inop->op_id==0)){
        if (d_seen(r, inop->console_id, inop->op_id)) return; /* уже видели */
    }

    /* Копируем payload'ы, т.к. wire освободит свои после колбэка. */
    ConOp op = *inop;
    void* copy_data=NULL; void* copy_init=NULL;
    if (data && dlen){ copy_data=malloc(dlen); if(copy_data){ memcpy(copy_data,data,dlen); op.data=copy_data; op.size=dlen; } }
    if (init && ilen){ copy_init=malloc(ilen); if(copy_init){ memcpy(copy_init,init,ilen); op.init_blob=copy_init; op.init_size=ilen; } }

    /* Отметить как увиденное (для не-snapshot). */
    if (!(op.init_blob && op.op_id==0)) d_insert(r, op.console_id, op.op_id);

    /* 1) локальная доставка */
    fanout_local(r, &op);
    /* 2) ретрансмит всем остальным пирам */
    for (int i=0;i<r->pn;i++){
        Peer* p = &r->peers[i];
        if (!p->cow || p==from) continue;
        cow1tcp_send(p->cow, &op);
    }
    free(copy_data);
    free(copy_init);
}

/* Принят новый inbound peer (после accept) */
static void on_listen(void* user, net_fd_t fd, int ev){
    CrdtMesh* r = (CrdtMesh*)user; if (!r) return;
    if (!(ev & NET_RD)) return;
    for (int budget=16; budget>0; --budget){
        tcp_fd_t cfd = (tcp_fd_t)TCP_INVALID_FD;
        int rc = tcp_accept((tcp_fd_t)fd, /*nb=*/1, &cfd, NULL, NULL);
        if (rc == 1) break;
        if (rc < 0) break;
        if (r->pn >= CRDT_MAX_PEERS){ tcp_close(cfd); continue; }
        Peer* p = &r->peers[r->pn++];
        memset(p,0,sizeof(*p));
        p->fd = (net_fd_t)cfd; p->owner = r; p->alive=1;
        p->cow = cow1tcp_create(r->np, p->fd, on_peer_op, p);
        /* Сразу отправим снапшоты известных топиков */
        send_snapshots_to_peer(r, p);
    }
}

/* ===== Outbound «seed» подключения ===== */
typedef struct PendingConn {
    net_fd_t  fd;
    CrdtMesh* owner;
} PendingConn;

static void on_connect(void* user, net_fd_t fd, int ev){
    PendingConn* pc = (PendingConn*)user;
    CrdtMesh* r = pc ? pc->owner : NULL;
    if (!r){ if (pc) free(pc); return; }
    if (!(ev & (NET_WR|NET_ERR))){ return; }
    /* Проверка завершения connect() */
    if (net_connect_finished(fd) != NET_OK){
        net_poller_del(r->np, fd);
        net_close_fd(fd);
        free(pc);
        return;
    }
    /* Успех — превращаем в Peer */
    net_poller_del(r->np, fd);
    if (r->pn >= CRDT_MAX_PEERS){ net_close_fd(fd); free(pc); return; }
    Peer* p = &r->peers[r->pn++];
    memset(p,0,sizeof(*p));
    p->fd = fd; p->owner=r; p->alive=1;
    p->cow = cow1tcp_create(r->np, p->fd, on_peer_op, p);
    /* Отправим снапшоты */
    send_snapshots_to_peer(r, p);
    free(pc);
}

static void dial_seed(CrdtMesh* r, const char* host, uint16_t port){
    if (!r || !host || !*host) return;
    tcp_fd_t sfd = (tcp_fd_t)TCP_INVALID_FD;
    int rc = tcp_connect(host, port, /*set_nb=*/1, &sfd);
    if (rc == NET_OK){
        /* Сразу оформим peer */
        if (r->pn >= CRDT_MAX_PEERS){ tcp_close(sfd); return; }
        Peer* p = &r->peers[r->pn++];
        memset(p,0,sizeof(*p));
        p->fd = (net_fd_t)sfd; p->owner=r; p->alive=1;
        p->cow = cow1tcp_create(r->np, p->fd, on_peer_op, p);
        send_snapshots_to_peer(r, p);
    } else if (rc == NET_INPROGRESS){
        PendingConn* pc = (PendingConn*)calloc(1,sizeof(*pc));
        if (!pc){ tcp_close(sfd); return; }
        pc->fd = (net_fd_t)sfd; pc->owner = r;
        net_poller_add(r->np, pc->fd, NET_WR|NET_ERR, on_connect, pc);
    } else {
        /* ошибка — ничего */
    }
}

/* ===== VTable реализация ===== */
static void cm_destroy(Replicator* rr){
    if (!rr) return;
    CrdtMesh* r = (CrdtMesh*)rr->impl;
    if (r){
        for (int i=0;i<r->pn;i++){
            if (r->peers[i].cow){ cow1tcp_destroy(r->peers[i].cow); r->peers[i].cow=NULL; }
            if ((intptr_t)r->peers[i].fd >= 0){
                net_poller_del(r->np, r->peers[i].fd);
                tcp_close((tcp_fd_t)r->peers[i].fd);
            }
        }
        if ((intptr_t)r->listen_fd >= 0){
            net_poller_del(r->np, (net_fd_t)r->listen_fd);
            tcp_close(r->listen_fd);
            r->listen_fd = (tcp_fd_t)TCP_INVALID_FD;
        }
        free(r->dedup);
        free(r);
    }
    free(rr);
}

static void cm_publish(Replicator* rr, const ConOp* op){
    if (!rr || !op) return;
    CrdtMesh* r = (CrdtMesh*)rr->impl;
    /* Скопировать payload’ы (контракт publish: копирует данные) */
    ConOp tmp = *op;
    void* copy_data=NULL; void* copy_init=NULL;
    if (op->data && op->size){ copy_data=malloc(op->size); if(copy_data){ memcpy(copy_data,op->data,op->size); tmp.data=copy_data; tmp.size=op->size; } }
    if (op->init_blob && op->init_size){ copy_init=malloc(op->init_size); if(copy_init){ memcpy(copy_init,op->init_blob,op->init_size); tmp.init_blob=copy_init; tmp.init_size=op->init_size; } }

    /* Отметить как увиденное — чтобы не зациклить самих себя. Снапшот не учитываем. */
    if (!(tmp.init_blob && tmp.op_id==0)) d_insert(r, tmp.console_id, tmp.op_id);

    /* 1) локально подтвердить */
    fanout_local(r, &tmp);
    /* 2) отправить всем пирами */
    for (int i=0;i<r->pn;i++){
        if (r->peers[i].cow) cow1tcp_send(r->peers[i].cow, &tmp);
    }
    free(copy_data);
    free(copy_init);
}

static void cm_set_listener(Replicator* rr, TopicId topic, ReplicatorConfirmCb cb, void* user){
    if (!rr || !cb) return;
    CrdtMesh* r = (CrdtMesh*)rr->impl;
    /* запомним топик — понадобится, чтобы отправлять снапшоты новым пирам */
    int have = 0;
    for (int i=0;i<r->tn;i++){
        if (r->topics[i].have && r->topics[i].t.type_id==topic.type_id && r->topics[i].t.inst_id==topic.inst_id){ have=1; break; }
    }
    if (!have && r->tn < CRDT_MAX_TOPICS){
        r->topics[r->tn].t = topic;
        r->topics[r->tn].have = 1;
        r->tn++;
    }
    /* зарегистрируем listener (с фильтром по inst_id~console_id для совместимости) */
    if (r->ln < CRDT_MAX_LISTENERS){
        r->ls[r->ln].cb = cb;
        r->ls[r->ln].user = user;
        r->ls[r->ln].inst_id = topic.inst_id; /* 0 — wildcard */
        r->ln++;
    }
}

static void cm_unset_listener(Replicator* rr, TopicId topic){
    if (!rr) return;
    CrdtMesh* r = (CrdtMesh*)rr->impl;
    uint64_t cid = topic.inst_id;
    int w=0;
    for (int i=0;i<r->ln;i++){
        if (!(r->ls[i].inst_id==cid || cid==0)) r->ls[w++] = r->ls[i];
    }
    r->ln = w;
}

static int cm_capabilities(Replicator* rr){
    (void)rr;
    return REPL_CRDT | REPL_BROADCAST;
}

static int cm_health(Replicator* rr){
    if (!rr) return -1;
    CrdtMesh* r = (CrdtMesh*)rr->impl;
    if (!r) return -1;
    /* Здоров, если слушаем порт или есть активные пиры */
    if ((intptr_t)r->listen_fd >= 0) return 0;
    if (r->pn > 0) return 0;
    return -1;
}

static const ReplicatorVt CM_VT = {
    .destroy      = cm_destroy,
    .publish      = cm_publish,
    .set_listener = cm_set_listener,
    .unset_listener = cm_unset_listener,
    .capabilities = cm_capabilities,
    .health       = cm_health,
};

/* ===== Фабрики ===== */
Replicator* replicator_create_crdt_mesh(NetPoller* np, uint16_t listen_port,
                                        const char** seeds, int nseeds)
{
    CrdtMesh* impl = (CrdtMesh*)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
    impl->np = np;
    impl->listen_fd = (tcp_fd_t)TCP_INVALID_FD;
    impl->port = listen_port;
    impl->dedup = NULL; impl->dcap = 0; impl->dcount = 0;
    impl->ln = impl->tn = impl->pn = 0;

    if (np && listen_port){
        tcp_fd_t lf = tcp_listen(listen_port, 64);
        if ((intptr_t)lf >= 0){
            impl->listen_fd = lf;
            net_poller_add(np, (net_fd_t)impl->listen_fd, NET_RD|NET_ERR, on_listen, impl);
        }
    }
    if (np && seeds && nseeds>0){
        for (int i=0;i<nseeds;i++){
            if (seeds[i] && *seeds[i]) dial_seed(impl, seeds[i], listen_port);
        }
    }

    Replicator* r = (Replicator*)calloc(1, sizeof(*r));
    if (!r){
        if ((intptr_t)impl->listen_fd >= 0){
            net_poller_del(np, (net_fd_t)impl->listen_fd);
            tcp_close(impl->listen_fd);
        }
        free(impl);
        return NULL;
    }
    r->v = &CM_VT;
    r->impl = impl;
    return r;
}

Replicator* replicator_create_crdt(void){
    /* Упрощённая «без сети» версия: np=NULL, не слушаем и не коннектимся.
       Полезно для тестов — будет работать как локальный CRDT-шлюз (фановт + дедуп). */
    CrdtMesh* impl = (CrdtMesh*)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
    impl->np = NULL;
    impl->listen_fd = (tcp_fd_t)TCP_INVALID_FD;
    impl->port = 0;
    impl->dedup = NULL; impl->dcap = 0; impl->dcount = 0;
    impl->ln = impl->tn = impl->pn = 0;

    Replicator* r = (Replicator*)calloc(1, sizeof(*r));
    if (!r){ free(impl); return NULL; }
    r->v = &CM_VT;
    r->impl = impl;
    return r;
}

int repl_crdt_mesh_seed_add(Replicator* rr, const char* host){
    if (!rr || !host || !*host) return -1;
    CrdtMesh* r = (CrdtMesh*)rr->impl; if (!r) return -1;
    /* используем текущий mesh-порт */
    dial_seed(r, host, r->port ? r->port : 0);
    return 0;
}

int repl_crdt_mesh_stat(Replicator* rr, int* out_peers, int* out_listen, int* out_topics){
    if (!rr) return -1;
    CrdtMesh* r = (CrdtMesh*)rr->impl; if (!r) return -1;
    if (out_peers)  *out_peers  = r->pn;
    if (out_listen) *out_listen = ((intptr_t)r->listen_fd >= 0) ? 1 : 0;
    if (out_topics) *out_topics = r->tn;
    return 0;
}
#endif /* !__EMSCRIPTEN__ */
