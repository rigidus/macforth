#pragma once
#include <stdint.h>
#include "replication/repl_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * In-proc «local loop» репликатор (fallback без сети).
     * Гарантии: ORDERED | RELIABLE | BROADCAST.
     */
    Replicator* replicator_create_local_loop(void);

#ifdef __cplusplus
} // extern "C"
#endif
