// tests/test_repl_hub.c
#include <assert.h>
#include <stdlib.h>
#include "replication/hub.h"
#include "replication/repl_iface.h"
#include "replication/repl_types.h"
#include "console/replicator.h"

/* TODO:хороший smoke, но не покрывает:
   - смену здоровья/health → перевыбор бэкенда;
   - мягкий replhub_switch_backend(...) + доставка буфера;
   - per-topic required_caps и force_backend.
   - Я бы добавил второй мини-тест: форс-свитч и проверку, что буфер отправился после разблокировки. */

typedef struct { int caps, health, publ, listened; ReplicatorConfirmCb cb; void* user; } Fake;
static void fk_destroy(Replicator* r){ free(r->impl); free(r); }
static void fk_publish(Replicator* r, const ConOp* op){ Fake* f=r->impl; f->publ++; if(f->cb) f->cb(f->user,op); }
static void fk_set(Replicator* r, TopicId t, ReplicatorConfirmCb cb, void* u){ (void)t; Fake* f=r->impl; f->listened=1; f->cb=cb; f->user=u; }
static int  fk_caps(Replicator* r){ return ((Fake*)r->impl)->caps; }
static int  fk_health(Replicator* r){ return ((Fake*)r->impl)->health; }
static const ReplicatorVt VT = { fk_destroy,fk_publish,fk_set,fk_caps,fk_health };
static Replicator* make_fake(int caps,int health){ Fake* f=calloc(1,sizeof* f); f->caps=caps; f->health=health; Replicator* r=calloc(1,sizeof* r); r->v=&VT; r->impl=f; return r; }

/* Глобальный счётчик подтверждений для простоты */
static int g_confirms = 0;
static void on_conf(void* u, const ConOp* op){ (void)u; (void)op; g_confirms++; }

int main(void){
    Replicator* leader = make_fake(REPL_ORDERED|REPL_RELIABLE|REPL_BROADCAST, /*bad*/1);
    Replicator* local  = make_fake(REPL_ORDERED|REPL_RELIABLE|REPL_BROADCAST, /*ok*/0);
    ReplBackendRef refs[2] = { {leader,100}, {local,10} };
    Replicator* hub = replicator_create_hub(refs, 2, 0, 0);
    assert(hub);

    TopicId t = (TopicId){ .type_id=1, .inst_id=42 };
    g_confirms = 0;
    replicator_set_listener(hub, t, on_conf, NULL);

    ConOp op = {0}; op.console_id = 42;
    replicator_publish(hub, &op);

    assert(((Fake*)local->impl)->publ == 1);
    assert(((Fake*)leader->impl)->publ == 0);
    assert(g_confirms == 1);

    replicator_destroy(hub);
    replicator_destroy(leader);
    replicator_destroy(local);
    return 0;
}
