#include "net/conop_wire.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ======== Низкоуровневые LE-хелперы ======== */

/* --- LE helpers (не зависят от архитектуры) --- */
static inline void wr8 (uint8_t** p, uint8_t v){ *(*p)++ = v; }
static inline void wr16(uint8_t** p, uint16_t v){
    uint8_t* d = *p; d[0]=(uint8_t)(v&0xFFu); d[1]=(uint8_t)((v>>8)&0xFFu); *p += 2;
}
static inline void wr32(uint8_t** p, uint32_t v){
    uint8_t* d = *p; d[0]=(uint8_t)(v); d[1]=(uint8_t)(v>>8); d[2]=(uint8_t)(v>>16); d[3]=(uint8_t)(v>>24); *p += 4;
}
static inline void wr64(uint8_t** p, uint64_t v){
    wr32(p, (uint32_t)(v & 0xFFFFFFFFull));
    wr32(p, (uint32_t)(v >> 32));
}
static inline uint8_t rd8 (const uint8_t** p){ uint8_t v = *(*p)++; return v; }
static inline uint16_t rd16(const uint8_t** p){ const uint8_t* s=*p; *p+=2; return (uint16_t)(s[0] | (uint16_t)s[1]<<8); }
static inline uint32_t rd32(const uint8_t** p){ const uint8_t* s=*p; *p+=4; return (uint32_t)s[0] | ((uint32_t)s[1]<<8) | ((uint32_t)s[2]<<16) | ((uint32_t)s[3]<<24); }
static inline uint64_t rd64(const uint8_t** p){ uint64_t lo=rd32(p), hi=rd32(p); return lo | (hi<<32); }

/* Постоянный размер сериализованного ConPosId: depth(1) + N*(2+4) */
static inline size_t pos_bytes(void){
    return 1u + (size_t)CON_POS_MAX_DEPTH * (2u + 4u);
}

static inline void wr_pos(uint8_t** p, const ConPosId* pos){
    uint8_t depth = pos ? pos->depth : 0;
    wr8(p, depth);
    for (int i=0;i<CON_POS_MAX_DEPTH;i++){
        uint16_t digit = (pos && i<pos->depth) ? pos->comp[i].digit : 0;
        uint32_t actor = (pos && i<pos->depth) ? pos->comp[i].actor : 0;
        wr16(p, digit);
        wr32(p, actor);
    }
}
static inline void rd_pos(const uint8_t** p, ConPosId* out){
    if (!out) return;
    memset(out, 0, sizeof(*out));
    uint8_t depth = rd8(p);
    if (depth > CON_POS_MAX_DEPTH) depth = CON_POS_MAX_DEPTH;
    out->depth = depth;
    for (int i=0;i<CON_POS_MAX_DEPTH;i++){
        uint16_t digit = rd16(p);
        uint32_t actor = rd32(p);
        if (i < depth){ out->comp[i].digit = digit; out->comp[i].actor = actor; }
    }
}

static inline size_t header_without_prefix_bytes(void){
    /* magic[4] + ver(2) +
       topic.type_id(8) + topic.inst_id(8) + schema(4) +
       type(2) +
       console_id(8) + op_id(8) + actor_id(4) + hlc(8) + user_id(4) +
       widget_id(8) + widget_kind(4) + new_item_id(8) + parent_left(8) + parent_right(8) +
       ConPosId (1 + N*(2+4)) +
       init_hash(8) + prompt_edits_inc(4) + prompt_nonempty(4) +
       tag_len(4) + data_len(4) + init_len(4)
    */
    return 4 + 2 +
        8 + 8 + 4 +
        2 +
        8 + 8 + 4 + 8 + 4 +
        8 + 4 + 8 + 8 + 8 +
        pos_bytes() +
        8 + 4 + 4 +
        4 + 4 + 4;
}

static int validate_lengths(uint32_t tag_len, uint32_t data_len, uint32_t init_len){
    return (tag_len  <= COW1_MAX_TAG) && (data_len <= COW1_MAX_DATA) && (init_len <= COW1_MAX_INIT);
}

int conop_wire_encode(const ConOp* op, uint8_t** out_buf, size_t* out_len){
    if (!op || !out_buf || !out_len) return -1;
    const uint32_t tag_len  = (op->tag && *op->tag) ? (uint32_t)strlen(op->tag) : 0u;
    const uint32_t data_len = (op->data && op->size) ? (uint32_t)op->size : 0u;
    const uint32_t init_len = (op->init_blob && op->init_size) ? (uint32_t)op->init_size : 0u;
    const size_t hdr = header_without_prefix_bytes();
    if (!validate_lengths(tag_len, data_len, init_len)) return -1;

    const uint32_t frame_payload_len = (uint32_t)(hdr + tag_len + data_len + init_len); /* после u32 длины */
    const size_t total = 4u /* frame prefix */ + frame_payload_len;

    /* Определяем topic для кодирования:
       если не задан (оба поля == 0), трактуем как консоль по console_id */
    TopicId topic = op->topic;
    if (topic.type_id == 0 && topic.inst_id == 0){
        topic.type_id = 1u;               /* TYPE_CONSOLE */
        topic.inst_id = op->console_id;   /* совместимость */
    }
    uint32_t schema = op->schema;

    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return -1;
    uint8_t* p = buf;
    /* frame length prefix (LE) */
    wr32(&p, frame_payload_len);
    /* magic + version */
    memcpy(p, CONOP_WIRE_MAGIC_STR, 4); p += 4;
    wr16(&p, (uint16_t)CONOP_WIRE_VERSION);
    /* header */
    wr64(&p, (uint64_t)topic.type_id);
    wr64(&p, (uint64_t)topic.inst_id);
    wr32(&p, (uint32_t)schema);
    wr16(&p, (uint16_t)op->type);
    wr64(&p, (uint64_t)op->console_id);
    wr64(&p, (uint64_t)op->op_id);
    wr32(&p, (uint32_t)op->actor_id);
    wr64(&p, (uint64_t)op->hlc);
    wr32(&p, (uint32_t)(int32_t)op->user_id);
    wr64(&p, (uint64_t)op->widget_id);
    wr32(&p, (uint32_t)op->widget_kind);
    wr64(&p, (uint64_t)op->new_item_id);
    wr64(&p, (uint64_t)op->parent_left);
    wr64(&p, (uint64_t)op->parent_right);
    wr_pos(&p, &op->pos);
    wr64(&p, (uint64_t)op->init_hash);
    wr32(&p, (uint32_t)(int32_t)op->prompt_edits_inc);
    wr32(&p, (uint32_t)(int32_t)op->prompt_nonempty);
    wr32(&p, tag_len);
    wr32(&p, data_len);
    wr32(&p, init_len);
    /* payload chunks */
    if (tag_len)  { memcpy(p, op->tag, tag_len);   p += tag_len; }
    if (data_len) { memcpy(p, op->data, data_len); p += data_len; }
    if (init_len) { memcpy(p, op->init_blob, init_len); p += init_len; }
    /* done */
    *out_buf = buf;
    *out_len = total;
    return 0;
}

int conop_wire_frame_ready(const uint8_t* buf, size_t len, size_t* out_frame_len){
    if (!buf || len < 4) return 0;
    const uint8_t* p = buf;
    uint32_t L = rd32(&p);
    size_t need = 4u + (size_t)L;
    if (out_frame_len) *out_frame_len = need;
    return 1;
}

int conop_wire_decode(const uint8_t* buf, size_t len,
                      ConOp* out_op,
                      char** out_tag,
                      void** out_data, size_t* out_data_len,
                      void** out_init, size_t* out_init_len)
{
    if (!buf || len < 4 || !out_op) return -1;
    const uint8_t* p = buf;
    uint32_t frame_len = rd32(&p);
    if (len < 4u + (size_t)frame_len) return -2; /* неполный кадр */
    const uint8_t* end = buf + 4u + (size_t)frame_len;

    /* magic */
    if ((size_t)(end - p) < 4) return -1;
    if (memcmp(p, CONOP_WIRE_MAGIC_STR, 4) != 0) return -1;
    p += 4;
    uint16_t ver = rd16(&p);
    if (ver != CONOP_WIRE_VERSION) return -1;

    ConOp op = {0};
    if ((size_t)(end - p) < header_without_prefix_bytes()) return -1;
    /* (ниже читаем поля; остатка точно достаточно) */

    op.topic.type_id = (uint64_t)rd64(&p);
    op.topic.inst_id = (uint64_t)rd64(&p);
    op.schema        = (uint32_t)rd32(&p);
    /* Защитa: если по каким-то причинам topic пуст — совместимость с "консолью" */
    if (op.topic.type_id == 0 && op.topic.inst_id == 0){
        op.topic.type_id = 1u; /* console */
        /* заполнится ниже из console_id, но инициализируем на всякий */
    }
    op.type        = (ConOpType)rd16(&p);
    op.console_id  = (uint64_t)rd64(&p);
    /* Синхронизируем inst_id с console_id, если inst_id не задан */
    if (op.topic.inst_id == 0) op.topic.inst_id = op.console_id;
    op.op_id       = (uint64_t)rd64(&p);
    op.actor_id    = (uint32_t)rd32(&p);
    op.hlc         = (uint64_t)rd64(&p);
    op.user_id     = (int32_t)rd32(&p);
    op.widget_id   = (uint64_t)rd64(&p);
    op.widget_kind = (uint32_t)rd32(&p);
    op.new_item_id = (uint64_t)rd64(&p);
    op.parent_left = (uint64_t)rd64(&p);
    op.parent_right= (uint64_t)rd64(&p);
    rd_pos(&p, &op.pos);
    op.init_hash   = (uint64_t)rd64(&p);
    op.prompt_edits_inc = (int32_t)rd32(&p);
    op.prompt_nonempty  = (int32_t)rd32(&p);
    uint32_t tag_len  = rd32(&p);
    uint32_t data_len = rd32(&p);
    uint32_t init_len = rd32(&p);

    /* базовая валидация длин и границ */
    if (!validate_lengths(tag_len, data_len, init_len)) return -1;
    /* границы */
    if ((size_t)(end - p) < (size_t)tag_len + (size_t)data_len + (size_t)init_len) return -1;

    /* лёгкая семантическая проверка по типам */
    if (op.type == CON_OP_PROMPT_META){
        if (tag_len || data_len || init_len) return -1;
    }
    if (op.type == CON_OP_INSERT_WIDGET){
        if (op.widget_kind == 0) return -1;
    }
    if (op.pos.depth > CON_POS_MAX_DEPTH) return -1;

    char* tag = NULL;
    void* data = NULL;
    void* init = NULL;

    if (tag_len){
        tag = (char*)malloc((size_t)tag_len + 1u);
        if (!tag) return -1;
        memcpy(tag, p, tag_len); tag[tag_len] = 0; p += tag_len;
        op.tag = tag;
    } else {
        op.tag = NULL;
    }
    if (data_len){
        data = malloc(data_len);
        if (!data){ free(tag); return -1; }
        memcpy(data, p, data_len); p += data_len;
        op.data = data; op.size = data_len;
    }
    if (init_len){
        init = malloc(init_len);
        if (!init){ free(tag); free(data); return -1; }
        memcpy(init, p, init_len); p += init_len;
        op.init_blob = init; op.init_size = init_len;
    }
    /* ok */
    *out_op = op;
    if (out_tag) *out_tag = tag; else free(tag);
    if (out_data){ *out_data = data; if (out_data_len) *out_data_len = data_len; }
    else { free(data); }
    if (out_init){ *out_init = init; if (out_init_len) *out_init_len = init_len; }
    else { free(init); }
    return 0;
}

void conop_wire_free_decoded(char* tag, void* data, void* init_blob){
    free(tag);
    free(data);
    free(init_blob);
}


/* ======== Потоковый декодер ======== */

void cow1_decoder_init(Cow1Decoder* d){
    if (!d) return;
    d->buf = NULL; d->len = d->cap = 0; d->want_frame_total = 0;
}

void cow1_decoder_reset(Cow1Decoder* d){
    if (!d) return;
    free(d->buf); d->buf = NULL; d->len = d->cap = 0; d->want_frame_total = 0;
}

static int ensure_cap(Cow1Decoder* d, size_t need){
    if (d->cap >= need) return 1;
    size_t ncap = d->cap ? d->cap : 4096;
    while (ncap < need) ncap = (ncap < (SIZE_MAX/2)) ? (ncap * 2) : need;
    void* nb = realloc(d->buf, ncap);
    if (!nb) return 0;
    d->buf = (uint8_t*)nb; d->cap = ncap; return 1;
}

size_t cow1_decoder_consume(Cow1Decoder* d, const uint8_t* data, size_t len){
    if (!d || !data || !len) return 0;
    if (!ensure_cap(d, d->len + len)) return 0;
    memcpy(d->buf + d->len, data, len);
    d->len += len;
    return len;
}

static int peek_total_len(const uint8_t* buf, size_t len, size_t* out_total){
    if (len < 4) return 0;
    const uint8_t* p = buf;
    uint32_t L = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
    size_t total = 4u + (size_t)L;
    if (total < 8) return 0; /* бессмысленно мало */
    if (out_total) *out_total = total;
    return 1;
}

/* сдвиг буфера на n байт влево */
static void drop_prefix(Cow1Decoder* d, size_t n){
    if (!d || n==0) return;
    if (n >= d->len){ d->len = 0; d->want_frame_total = 0; return; }
    memmove(d->buf, d->buf + n, d->len - n);
    d->len -= n;
    d->want_frame_total = 0;
}

int cow1_decoder_take_next(Cow1Decoder* d,
                           ConOp* out_op,
                           char** out_tag,
                           void** out_data, size_t* out_data_len,
                           void** out_init, size_t* out_init_len)
{
    if (!d || !out_op) return -1;
    if (d->len < 4){
        d->want_frame_total = 0;
        return 0;
    }
    if (d->want_frame_total == 0){
        size_t total = 0;
        if (!peek_total_len(d->buf, d->len, &total)) return 0;
        d->want_frame_total = total;
    }
    if (d->len < d->want_frame_total) return 0;
    /* у нас есть полный кадр */
    int rc = conop_wire_decode(d->buf, d->want_frame_total, out_op, out_tag, out_data, out_data_len, out_init, out_init_len);
    if (rc != 0){
        /* невалидный кадр — сбрасываем накопленное */
        d->len = 0;
        d->want_frame_total = 0;
        return -2;
    }
    drop_prefix(d, d->want_frame_total);
    return 1;
}
