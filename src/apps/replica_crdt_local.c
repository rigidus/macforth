#include "console/replicator.h"
/* Заглушка «gossip между двумя Store в одном процессе».
   Пока идентично authoritative_local — сразу подтверждает. */

/* Реализация переиспользует символы из replica_authoritative_local.c через интерфейс;
   для простоты экспортируем только фабрику. */

extern Replicator* replicator_create_authoritative_local(void);

Replicator* replicator_create_crdt_local_stub(void){
    return replicator_create_authoritative_local();
}
