#pragma once
#include <stdint.h>
#include "replication/repl_iface.h"   // Replicator, ReplicatorVt

#ifdef __cplusplus
extern "C" {
#endif

    // Форвард-декларация, чтобы не тянуть net заголовки в публичный API бэкенда
    typedef struct NetPoller NetPoller;

    /**
     * Создаёт TCP-leader бэкенд.
     * @param np         готовый поллер
     * @param console_id идентификатор "консоли" (для HELO/WLCM)
     * @param port       порт для прослушивания
     * @return           Replicator* с vtable LEADER_VT или NULL при ошибке
     *
     * На Emscripten возвращает NULL (стаб).
     */
#if defined(__EMSCRIPTEN__)
    static inline Replicator* replicator_create_leader_tcp(NetPoller* np, uint64_t console_id, uint16_t port){
        (void)np; (void)console_id; (void)port; return NULL;
    }
#else
    Replicator* replicator_create_leader_tcp(NetPoller* np, uint64_t console_id, uint16_t port);
#endif

#ifdef __cplusplus
} // extern "C"
#endif
