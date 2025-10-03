    /* In-proc «local loop» backend: простой event-bus в рамках процесса.
     * Реализация под общий интерфейс ReplicatorVt (replication/repl_iface.h).
     */
#include "replication/backends/local_loop.h"
#include "replication/repl_iface.h"
#include "replication/repl_types.h"
#include "common/conop.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef REPL_MAX_LISTENERS
#  define REPL_MAX_LISTENERS 16
#endif

typedef struct Listener {
    ReplicatorConfirmCb cb;
    void* user;
    uint64_t cid; /* фильтр по op->console_id; 0 = wildcard */
} Listener;

typedef struct LocalLoop {
    Listener ls[REPL_MAX_LISTENERS];
    int ln;
} LocalLoop;

/* ===== fanout ===== */
static void fanout(LocalLoop* r, const ConOp* op){
    if (!r || !op) return;
    for (int i=0;i<r->ln;i++){
        Listener* L = &r->ls[i];
        if (L->cb && (L->cid==0 || L->cid==op->console_id)){
            L->cb(L->user, op);
        }
    }
}

/* ===== VTable реализация ===== */
static void ll_destroy(Replicator* rr){
    if (!rr) return;
    LocalLoop* r = (LocalLoop*)rr->impl;
    free(r);
    free(rr);
}

static void ll_publish(Replicator* rr, const ConOp* op){
    if (!rr || !op) return;
    LocalLoop* r = (LocalLoop*)rr->impl;
    /* Клонируем payload’ы, чтобы соблюсти контракт «publish копирует данные». */
    ConOp tmp = *op;
    void* copy_data = NULL;
    void* copy_init = NULL;
    if (op->data && op->size){
        copy_data = malloc(op->size);
        if (copy_data){ memcpy(copy_data, op->data, op->size); tmp.data = copy_data; tmp.size = op->size; }
    }
    if (op->init_blob && op->init_size){
        copy_init = malloc(op->init_size);
        if (copy_init){ memcpy(copy_init, op->init_blob, op->init_size); tmp.init_blob = copy_init; tmp.init_size = op->init_size; }
    }
    fanout(r, &tmp);
    free(copy_data);
    free(copy_init);
}

static void ll_set_listener(Replicator* rr, TopicId topic, ReplicatorConfirmCb cb, void* user){
    if (!rr || !cb) return;
    LocalLoop* r = (LocalLoop*)rr->impl;
    if (r->ln >= REPL_MAX_LISTENERS) return;
    r->ls[r->ln].cb = cb;
    r->ls[r->ln].user = user;
    /* для совместимости: фильтруем по console_id; используем topic.inst_id */
    r->ls[r->ln].cid = topic.inst_id; /* 0 — wildcard */
    r->ln++;
}

static void ll_unset_listener(Replicator* rr, TopicId topic){
    if (!rr) return;
    LocalLoop* r = (LocalLoop*)rr->impl;
    /* фильтр совпадал по inst_id; достаточно убрать все с таким cid */
    uint64_t cid = topic.inst_id;
    int w=0;
    for (int i=0;i<r->ln;i++){
        if (!(r->ls[i].cid==cid || cid==0)) r->ls[w++] = r->ls[i];
    }
    r->ln = w;
}

static int ll_capabilities(Replicator* rr){
    (void)rr;
    return REPL_ORDERED | REPL_RELIABLE | REPL_BROADCAST;
}

static int ll_health(Replicator* rr){
    (void)rr;
    return 0; /* всегда OK */
}

static const ReplicatorVt LOCAL_VT = {
    .destroy      = ll_destroy,
    .publish      = ll_publish,
    .set_listener = ll_set_listener,
    .unset_listener = ll_unset_listener,
    .capabilities = ll_capabilities,
    .health       = ll_health,
};

/* ===== Фабрика ===== */
Replicator* replicator_create_local_loop(void){
    LocalLoop* impl = (LocalLoop*)calloc(1, sizeof(*impl));
    if (!impl) return NULL;
    Replicator* r = (Replicator*)calloc(1, sizeof(*r));
    if (!r){ free(impl); return NULL; }
    r->v    = &LOCAL_VT;
    r->impl = impl;
    return r;
}
