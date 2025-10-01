#include "console/replicator.h"

/* Локальный CRDT-«gossip»: несколько слушателей, рассылка всем.
   Копирует payload, идемпотентно сообщает всем подписчикам, включая эмитента. */

#include <stdlib.h>
#include <string.h>

struct Listener {
    ReplicatorConfirmCb cb;
    void* user;
    ConsoleId cid; /* подписка на конкретную консоль; 0 = wildcard */
};

struct Replicator {
    struct Listener ls[REPL_MAX_LISTENERS];
    int n;
};

static void fanout_publish(Replicator* r, const ConOp* op){
    if (!r || !op) return;
    for (int i=0;i<r->n;i++){
        if (r->ls[i].cb && (r->ls[i].cid==0 || r->ls[i].cid==op->console_id)){
            r->ls[i].cb(r->ls[i].user, op);
        }
    }
}

Replicator* replicator_create_crdt_local(void){
    Replicator* r = (Replicator*)calloc(1, sizeof(Replicator));
    return r;
}

void replicator_destroy(Replicator* r){
    if (!r) return;
    free(r);
}

void replicator_set_confirm_listener(Replicator* r, ConsoleId console_id, ReplicatorConfirmCb cb, void* user){
    if (!r || !cb) return;
    if (r->n < REPL_MAX_LISTENERS){
        r->ls[r->n].cb = cb;
        r->ls[r->n].user = user;
        r->ls[r->n].cid  = console_id;
        r->n++;
    }
}

void replicator_publish(Replicator* r, const ConOp* op){
    if (!r || !op) return;
    /* Копируем payload’ы (data/init_blob) — готово к асинхронной доставке. */
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
    /* init_hash просто копируется по значению в tmp */
    fanout_publish(r, &tmp);
    free(copy_data);
    free(copy_init);
}
