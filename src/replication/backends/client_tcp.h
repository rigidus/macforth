#pragma once
#include <stdint.h>
#include "replication/repl_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* forward, чтобы не тянуть net.* в публичный хедер */
    typedef struct NetPoller NetPoller;

    /**
     * TCP-клиент к лидеру:
     * - неблокирующий connect к host:port, HELO/WLCM (протокол как у leader_tcp);
     * - после рукопожатия — поток COW1 (ConOp);
     * - локальная доставка confirm’ов слушателям;
     * - небольшой буфер publish до установления соединения.
     *
     * capabilities(): REPL_ORDERED | REPL_RELIABLE
     * health(): 0 если подключён, иначе !=0
     */
#if defined(__EMSCRIPTEN__)
    static inline Replicator* replicator_create_client_tcp(NetPoller* np, uint64_t console_id,
                                                           const char* host, uint16_t port)
    { (void)np; (void)console_id; (void)host; (void)port; return NULL; }
    static inline int  repl_client_tcp_connect(Replicator* r, const char* host, uint16_t port)
    { (void)r; (void)host; (void)port; return -1; }
    static inline void repl_client_tcp_disconnect(Replicator* r){ (void)r; }
    static inline int  repl_client_tcp_is_connected(Replicator* r){ (void)r; return 0; }
#else
    Replicator* replicator_create_client_tcp(NetPoller* np, uint64_t console_id,
                                             const char* host /* может быть NULL */, uint16_t port);
    int  repl_client_tcp_connect(Replicator* r, const char* host, uint16_t port);
    void repl_client_tcp_disconnect(Replicator* r);
    int  repl_client_tcp_is_connected(Replicator* r);
#endif

#ifdef __cplusplus
} // extern "C"
#endif
