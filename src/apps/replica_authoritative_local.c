#include "console/replicator.h"
#include <stdlib.h>
#include <string.h>

/* Репликатор поддерживает только одного «слушателя» */

/* Сейчас у Replicator один callback. Если позже понадобится несколько подписчиков (несколько вью/нод), лучше сделать список слушателей.  */

struct Replicator {
    ReplicatorConfirmCb cb;
    void* user;
    ConsoleId cid; /* 0 = wildcard */
};

static void loop_publish(Replicator* r, const ConOp* op){
    /* Локальный «лидер»: подтверждаем сразу же (синхронно, без копий) */
    if (r && r->cb && op && (r->cid==0 || r->cid==op->console_id)) r->cb(r->user, op);
}

Replicator* replicator_create_authoritative_local(void){
    Replicator* r = (Replicator*)calloc(1, sizeof(Replicator));
    return r;
}

void replicator_destroy(Replicator* r){
    if (!r) return;
    free(r);
}

void replicator_set_confirm_listener(Replicator* r, ConsoleId console_id, ReplicatorConfirmCb cb, void* user){
    if (!r) return;
    r->cb = cb; r->user = user; r->cid = console_id;
}

void replicator_publish(Replicator* r, const ConOp* op){
    /* Сейчас ConOp.data часто указывает на стековые буферы
       (например, в con_sink_commit() и submit_text()).
       Это работает потому, что loopback-репликатор подтверждает
       синхронно.
       Как только появится асинхронная репликация - use-after-free.
       Поэтому документируем, что replicator_publish()
       всегда копирует payload (malloc/free внутри репликатора),
    */
    if (!r || !op) return;
    ConOp tmp = *op;
    void* copy = NULL;
    void* copy2 = NULL;
    if (op->data && op->size){
        copy = malloc(op->size);
        if (copy){ memcpy(copy, op->data, op->size); tmp.data = copy; }
    }
    if (op->init_blob && op->init_size){
        copy2 = malloc(op->init_size);
        if (copy2){ memcpy(copy2, op->init_blob, op->init_size); tmp.init_blob = copy2; }
    }
    /* init_hash копируется по значению */
    loop_publish(r, &tmp);
    free(copy);
    free(copy2);
}
