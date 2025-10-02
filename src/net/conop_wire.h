#pragma once
#include <stddef.h>
#include <stdint.h>
#include "console/replicator.h"  /* ConOp, ConOpType */

#ifdef __cplusplus
extern "C" {
#endif

    /* Магия/версия wire-формата */
#define CONOP_WIRE_MAGIC_STR "COW1"
#define CONOP_WIRE_VERSION   1u

    /* Жёсткие лимиты секций (можно переопределить при сборке) */
    #ifndef COW1_MAX_TAG
    #  define COW1_MAX_TAG  (4u * 1024u)
    #endif
    #ifndef COW1_MAX_DATA
    #  define COW1_MAX_DATA (256u * 1024u)
    #endif
    #ifndef COW1_MAX_INIT
    #  define COW1_MAX_INIT (1024u * 1024u)
    #endif

    /* Кодирование одного ConOp в непрерывный кадр.
       Формат кадра:
       u32   frame_len_le   // длина всего кадра ПОСЛЕ этого поля
       char  magic[4] = "COW1"
       u16   ver = 1
       u16   type
       u64   console_id
       u64   op_id
       u32   actor_id
       u64   hlc
       i32   user_id
       u64   widget_id
       u32   widget_kind
       u64   new_item_id
       u64   parent_left
       u64   parent_right
       u8    pos.depth
       повторить CON_POS_MAX_DEPTH раз:
       u16 digit
       u32 actor
       u64   init_hash
       i32   prompt_edits_inc
       i32   prompt_nonempty
       u32   tag_len
       u32   data_len
       u32   init_len
       u8[tag_len]  tag (без завершающего 0)
       u8[data_len] data
       u8[init_len] init_blob
       Возврат 0 при успехе. Буфер выделяется внутри и должен быть освобождён
       через free() вызывающей стороной. out_len включает и префикс длины. */
    int conop_wire_encode(const ConOp* op, uint8_t** out_buf, size_t* out_len);

    /* Декодирование одного ПОЛНОГО кадра из buf,len.
       Возвращает 0 при успехе и заполняет:
       - out_op: скалярные поля + указатели на выделенные копии ниже,
       - out_tag: '\0'-terminated (может быть NULL, если tag_len==0),
       - out_data/out_data_len: копия payload (может быть NULL/0),
       - out_init/out_init_len: копия init_blob (может быть NULL/0).
       Освобождение через conop_wire_free_decoded(). */
    int conop_wire_decode(const uint8_t* buf, size_t len,
                          ConOp* out_op,
                          char** out_tag,
                          void** out_data, size_t* out_data_len,
                          void** out_init, size_t* out_init_len);

    /* Утилита для стриминга по TCP:
       - если в буфере <4 байт, вернёт 0 и out_frame_len не трогает;
       - если >=4, положит в *out_frame_len полную длину КАДРА С ПРЕФИКСОМ,
       и вернёт 1 (даже если самого кадра ещё не хватает).
       Это можно использовать, чтобы понять, сколько байт надо дочитать. */
    int conop_wire_frame_ready(const uint8_t* buf, size_t len, size_t* out_frame_len);

    /* Освобождение выделенных копий, полученных из conop_wire_decode(). */
    void conop_wire_free_decoded(char* tag, void* data, void* init_blob);

    /* ===== Потоковый декодер (для TCP) ===== */
    typedef struct {
        uint8_t* buf;   /* накопитель (с префиксами) */
        size_t   len;   /* фактически в буфере */
        size_t   cap;   /* вместимость */
        /* кэш известной длины кадра (включая префикс), 0 если неизвестна */
        size_t   want_frame_total;
    } Cow1Decoder;

    /* Инициализировать/сбросить */
    void   cow1_decoder_init(Cow1Decoder* d);
    void   cow1_decoder_reset(Cow1Decoder* d);

    /* Скопировать в декодер кусок байтового потока.
       Возвращает количество «съеденных» из input (всегда == len). */
    size_t cow1_decoder_consume(Cow1Decoder* d, const uint8_t* data, size_t len);

    /* Если полный кадр накоплен — разобрать и вернуть 1.
       На успехе выделяет копии tag/data/init (как conop_wire_decode()).
       Если кадра ещё нет — вернёт 0.
       При ошибке валидации вернёт <0 и сбросит внутренний буфер. */
    int    cow1_decoder_take_next(Cow1Decoder* d,
                                  ConOp* out_op,
                                  char** out_tag,
                                  void** out_data, size_t* out_data_len,
                                  void** out_init, size_t* out_init_len);


#ifdef __cplusplus
}
#endif
