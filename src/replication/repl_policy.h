#pragma once
#include "replication/repl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Неполное объявление, чтобы избежать жёсткой зависимости от console/replicator.h */
    struct ConOp;

    /* «Вид» бэкенда, который хаб предоставляет политике (без указателей/вызовов). */
    typedef struct ReplBackendView {
        int kind;   /* enum ReplKind из repl_hub.h */
        int caps;   /* ReplCaps */
        int health; /* 0 = OK, !=0 — деградация */
    } ReplBackendView;

    typedef struct ReplPolicy ReplPolicy; /* зарезервировано на будущее */

    /* Создать/уничтожить дефолтную политику (пока stateless). */
    ReplPolicy* repl_policy_create_default(void);
    void        repl_policy_destroy_default(ReplPolicy*);

    /* Выбор бэкенда по умолчанию.
       required_caps — битовая маска требований (0 — без требований). */
    int repl_policy_choose_default(const struct ConOp* op,
                                   int required_caps,
                                   const ReplBackendView* backends, int nbackends);

#ifdef __cplusplus
}
#endif
