#pragma once
#include "replication/repl_types.h"  /* ReplicatorConfirmCb, TopicId, ReplCaps */

#ifdef __cplusplus
extern "C" {
#endif

    struct ConOp;

    // Универсальная ручка репликатора через таблицу методов
    typedef struct Replicator Replicator;
    typedef struct ReplicatorVt ReplicatorVt;

    struct Replicator {
        const ReplicatorVt* v;
        void* impl;  // приватная реализация конкретного бэкенда
    };

    struct ReplicatorVt {
        void (*destroy)(Replicator*);
        void (*publish)(Replicator*, const struct ConOp*);
        void (*set_listener)(Replicator*, TopicId, ReplicatorConfirmCb, void* user);
        void (*unset_listener)(Replicator*, TopicId);
        int  (*capabilities)(Replicator*); // битовая маска ReplCaps
        int  (*health)(Replicator*);       // 0=OK, !=0 — код деградации
    };

// Удобные thin-wrappers
    static inline void replicator_destroy(Replicator* r){ if(r && r->v && r->v->destroy) r->v->destroy(r); }

    static inline void replicator_publish(Replicator* r, const struct ConOp* op){
        if (r && r->v && r->v->publish) r->v->publish(r, op);
    }

    static inline void replicator_set_listener(Replicator* r, TopicId t, ReplicatorConfirmCb cb, void* u){
        if(r && r->v && r->v->set_listener) r->v->set_listener(r, t, cb, u);
    }

    static inline void replicator_unset_listener(Replicator* r, TopicId t){
        if (r && r->v && r->v->unset_listener) r->v->unset_listener(r, t);
    }

    static inline int  replicator_capabilities(Replicator* r){ return (r && r->v && r->v->capabilities) ? r->v->capabilities(r) : 0; }

    static inline int  replicator_health(Replicator* r){ return (r && r->v && r->v->health) ? r->v->health(r) : -1; }

#ifdef __cplusplus
}
#endif
