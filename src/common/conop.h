/* Общий (консоль-независимый) тип операции репликации.
 * Содержит ConOpType, ConPosId и ConOp. Используется из net/, replication/, console/
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "replication/repl_types.h"  /* TopicId */

#ifdef __cplusplus
extern "C" {
#endif

    /* ======================= CRDT позиция ======================= */
#ifndef CON_POS_MAX_DEPTH
#  define CON_POS_MAX_DEPTH 8
#endif

    typedef struct ConPosId {
        uint8_t  depth;
        struct { uint16_t digit; uint32_t actor; } comp[CON_POS_MAX_DEPTH];
    } ConPosId;

    /* ======================= Типы операций ======================= */
    typedef enum ConOpType {
        /* CRDT вставка текста между якорями/new_id */
        CON_OP_INSERT_TEXT   = 1,
        /* CRDT вставка виджета (init_blob, widget_kind, new_id, pos/parents) */
        CON_OP_INSERT_WIDGET = 2,
        /* Сообщение конкретному виджету (widget_id + tag/data) */
        CON_OP_WIDGET_MSG    = 3,
        /* Дельта конкретному виджету (widget_id + tag/data) */
        CON_OP_WIDGET_DELTA  = 4,
        /* Готовая строка вывода (выход процессора) */
        CON_OP_APPEND_LINE   = 5,
        /* Метаданные промпта (edits_inc, nonempty) */
        CON_OP_PROMPT_META   = 6,
    } ConOpType;

    /* ======================= Описание операции ======================= */
    typedef struct ConOp {
        /* идентификатор темы и схема полезной нагрузки */
        TopicId  topic;     /* .type_id=1 (console) по умолчанию, .inst_id = console_id */
        uint32_t schema;    /* версия/вариант формата data/init (0 — по умолчанию) */

        /* ---- заголовок (строго в этом порядке; см. conop_wire.c) ---- */
        ConOpType  type;         /* u16 */
        uint64_t   console_id;   /* u64 : тема / инстанс */
        uint64_t   op_id;        /* u64 : монотонный id операции у автора */
        uint32_t   actor_id;     /* u32 : автор */
        uint64_t   hlc;          /* u64 : Hybrid Logical Clock */
        int32_t    user_id;      /* i32 : локальный пользователь (для UI) */

        uint64_t   widget_id;    /* u64 : целевой виджет (для MSG/DELTA) */
        uint32_t   widget_kind;  /* u32 : вид вставляемого виджета */
        uint64_t   new_item_id;  /* u64 : id нового элемента (вставки) */
        uint64_t   parent_left;  /* u64 : левый якорь */
        uint64_t   parent_right; /* u64 : правый якорь (или 0) */
        ConPosId   pos;          /* CRDT позиция */

        uint64_t   init_hash;        /* u64 : контент-хэш init_blob */
        int32_t    prompt_edits_inc; /* i32 : метаданные промпта */
        int32_t    prompt_nonempty;  /* i32 : метаданные промпта */

        /* ---- полезные нагрузки (вне wire-заголовка) ---- */
        const char*  tag;        /* опционально: '\0'-terminated */
        const void*  data;       /* payload */
        size_t       size;       /* длина payload */

        const void*  init_blob;  /* инициализатор для вставки виджета */
        size_t       init_size;  /* длина init_blob */
    } ConOp;

#ifdef __cplusplus
} /* extern "C" */
#endif
