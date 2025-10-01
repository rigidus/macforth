#pragma once
#include <stddef.h>
#include <stdint.h>
#include "store.h" /* ConItemId */
#define REPL_MAX_LISTENERS 16

#ifdef __cplusplus
extern "C" {
#endif

    typedef uint64_t ConsoleId; /* идентификатор консоли/ленты */

    typedef enum {
        CON_OP_APPEND_LINE  = 4, /* добавить готовую строку; payload: UTF-8 */
        CON_OP_WIDGET_MSG   = 5,
        CON_OP_WIDGET_DELTA = 6,
        CON_OP_INSERT_TEXT  = 7, /* CRDT-вставка текста по позиции */
        CON_OP_INSERT_WIDGET= 8  /* CRDT-вставка виджета по позиции */
    } ConOpType;

    typedef struct {
        uint64_t   op_id;      /* для идемпотентности (уникален в рамках узла, см. actor_id) */
        uint64_t   hlc;        /* Hybrid Logical Clock (подготовка к CRDT/LWW) */
        uint32_t   actor_id;   /* идентификатор эмитента (узла/актора) */
        ConsoleId  console_id; /* КУДА направлена операция (маршрутизация) */
        int        user_id;    /* источник (зарезервировано) */
        ConOpType  type;
        ConItemId  widget_id;  /* для widget_* */
        const char* tag;       /* для widget_* */
        const void* data;      /* произвольный payload (UTF-8 или blob) */
        size_t     size;
        /* ---- CRDT вставки ---- */
        /* новые элементы получают глобальный ID от эмитента */
        ConItemId  new_item_id;    /* ID вставляемого элемента */
        ConItemId  parent_left;    /* «левый» сосед (может быть 0) */
        ConItemId  parent_right;   /* «правый» сосед (может быть 0) */
        ConPosId   pos;            /* итоговая позиция (детерминированная) */
        /* для INSERT_WIDGET: тип и инициализационный blob */
        uint32_t   widget_kind;    /* 1=ColorSlider, … */
        const void* init_blob;
        size_t      init_size;
        uint64_t    init_hash;     /* контент-адрес (FNV-1a 64) для init_blob */
    } ConOp;

    typedef struct Replicator Replicator;
    typedef void (*ReplicatorConfirmCb)(void* user, const ConOp* op);

    /* Общий интерфейс */
    void replicator_destroy(Replicator*);
    void replicator_set_confirm_listener(Replicator*, ConsoleId console_id, ReplicatorConfirmCb cb, void* user);
    /* publish гарантированно копирует payload'ы (data/init_blob), см. реализации */
    void replicator_publish(Replicator*, const ConOp* op);

    /* Конкретные «бекенды» */
    Replicator* replicator_create_authoritative_local(void);
    Replicator* replicator_create_crdt_local(void); /* локальный gossip с несколькими слушателями */


#ifdef __cplusplus
}
#endif
