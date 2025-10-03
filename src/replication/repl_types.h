#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t type_id;   // тип компонента (CONSOLE=1, TEXT=2, ...)
    uint64_t inst_id;   // экземпляр
} TopicId;

/* forward declaration to avoid include cycle with conop.h */
typedef struct ConOp ConOp;

typedef void (*ReplicatorConfirmCb)(void* user, const ConOp* op);

/* Битовая маска возможностей бэкенда репликации */
typedef enum {
    REPL_ORDERED   = 1 << 0,  /* сохраняет порядок */
    REPL_RELIABLE  = 1 << 1,  /* гарантирует доставку/ретрай */
    REPL_BROADCAST = 1 << 2,  /* умеет фановт на несколько подписчиков */
    REPL_CRDT      = 1 << 3,  /* поддерживает CRDT-поток/конвергенцию */
    REPL_DISCOVER  = 1 << 4,  /* авто-дискавери (мультикаст/сервер) */
} ReplCaps;
