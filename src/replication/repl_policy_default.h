#pragma once
#include "repl_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct {
        Replicator* r;
        int         priority;
        int         caps;
        int         health; /* 0=OK, !=0 bad */
    } PolicyCandidate;

    /* Выбор лучшего индекса среди кандидатов:
       - отфильтровать health!=0 и caps & required == required;
       - выбрать с максимальным priority;
       - при равенстве — с наибольшими caps (больше возможностей);
       Возврат: индекс [0..n) или -1 если никого. */
    int repl_policy_default_choose(const PolicyCandidate* a, int n, int required_caps);

#ifdef __cplusplus
}
#endif
