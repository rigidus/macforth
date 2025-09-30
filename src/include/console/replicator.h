#pragma once
#include <stddef.h>
#include <stdint.h>
#include "store.h" /* ConItemId */

#ifdef __cplusplus
extern "C" {
#endif

    typedef enum {
        CON_OP_PUT_TEXT     = 1,
        CON_OP_BACKSPACE    = 2,
        CON_OP_COMMIT       = 3, /* перенос edit в историю; payload: UTF-8 строки (опц.) */
        CON_OP_APPEND_LINE  = 4, /* добавить готовую строку; payload: UTF-8 */
        CON_OP_WIDGET_MSG   = 5,
        CON_OP_WIDGET_DELTA = 6
    } ConOpType;

    typedef struct {
        uint64_t   op_id;     /* для идемпотентности */
        int        user_id;   /* источник (зарезервировано) */
        ConOpType  type;
        ConItemId  widget_id; /* для widget_* */
        const char* tag;      /* для widget_* */
        const void* data;     /* произвольный payload (UTF-8 или blob) */
        size_t     size;
    } ConOp;

    typedef struct Replicator Replicator;
    typedef void (*ReplicatorConfirmCb)(void* user, const ConOp* op);

    /* Общий интерфейс */
    void replicator_destroy(Replicator*);
    void replicator_set_confirm_listener(Replicator*, ReplicatorConfirmCb cb, void* user);
    void replicator_publish(Replicator*, const ConOp* op);

    /* Конкретные «бекенды» (M8 без сети) */
    Replicator* replicator_create_authoritative_local(void);
    Replicator* replicator_create_crdt_local_stub(void); /* заглушка */

#ifdef __cplusplus
}
#endif
