#pragma once
#include <stdint.h>
#include "replication/repl_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Форвард, чтобы не тянуть net-заголовки во внешний API */
    typedef struct NetPoller NetPoller;

    /**
     * CRDT mesh backend (TCP mesh):
     * - каждый узел может слушать порт (listen_port>0) и/или подключаться к seed-узлам;
     * - операции доставляются всем пирами, дедуплицируются по (console_id,op_id);
     * - при установлении соединения отправляется снапшот для всех подписанных топиков (через TypeRegistry).
     *
     * capabilities(): REPL_CRDT | REPL_BROADCAST
     * health(): 0, если есть listen_fd или хотя бы одно активное подключение.
     *
     * Замечания:
     *  - порядок не гарантируется (это CRDT-канал);
     *  - надёжность «на лучшем усилии»: при потере связи новые пиры получат актуальное
     *    состояние через снапшот при переподключении.
     */
#if defined(__EMSCRIPTEN__)
    static inline Replicator* replicator_create_crdt_mesh(NetPoller* np, uint16_t listen_port,
                                                          const char** seeds, int nseeds){
        (void)np; (void)listen_port; (void)seeds; (void)nseeds; return NULL;
    }
    static inline Replicator* replicator_create_crdt(void){ return NULL; }
    static inline int  repl_crdt_mesh_seed_add(Replicator* r, const char* host){ (void)r; (void)host; return -1; }
    static inline int  repl_crdt_mesh_stat(Replicator* r, int* out_peers, int* out_listen, int* out_topics){
        (void)r; if(out_peers)*out_peers=0; if(out_listen)*out_listen=0; if(out_topics)*out_topics=0; return -1;
    }
#else
    Replicator* replicator_create_crdt_mesh(NetPoller* np, uint16_t listen_port,
                                            const char** seeds, int nseeds);
    /* Удобная обёртка без сети (ни слушателя, ни сидов) — полезна для тестов. */
    Replicator* replicator_create_crdt(void);
    /* Runtime добавление seed и статистика (для консольных команд/диагностики). */
    int  repl_crdt_mesh_seed_add(Replicator* r, const char* host);
    int  repl_crdt_mesh_stat(Replicator* r, int* out_peers, int* out_listen, int* out_topics);
#endif

#ifdef __cplusplus
} // extern "C"
#endif
