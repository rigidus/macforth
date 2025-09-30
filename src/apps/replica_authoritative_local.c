#include "console/replicator.h"
#include <stdlib.h>
#include <string.h>

struct Replicator {
    ReplicatorConfirmCb cb;
    void* user;
};

static void loop_publish(Replicator* r, const ConOp* op){
    /* Локальный «лидер»: подтверждаем сразу же (синхронно, без копий) */
    if (r && r->cb && op) r->cb(r->user, op);
}

Replicator* replicator_create_authoritative_local(void){
    Replicator* r = (Replicator*)calloc(1, sizeof(Replicator));
    return r;
}

void replicator_destroy(Replicator* r){
    if (!r) return;
    free(r);
}

void replicator_set_confirm_listener(Replicator* r, ReplicatorConfirmCb cb, void* user){
    if (!r) return;
    r->cb = cb; r->user = user;
}

void replicator_publish(Replicator* r, const ConOp* op){
    loop_publish(r, op);
}
