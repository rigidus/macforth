#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "net/conop_wire.h"

static ConOp make_op_basic(void){
    ConOp o; memset(&o, 0, sizeof(o));
    o.topic.type_id = 1; /* console */
    o.topic.inst_id = 42;
    o.schema = 0;
    o.type = CON_OP_INSERT_TEXT;
    o.console_id = 42;
    o.op_id = 123456789ull;
    o.actor_id = 0xAABBCCDDu;
    o.hlc = 987654321ull;
    o.user_id = 1;
    o.widget_id = 0;
    o.widget_kind = 0;
    o.new_item_id = 1111;
    o.parent_left = 10;
    o.parent_right = 20;
    o.pos.depth = 2;
    o.pos.comp[0].digit = 100; o.pos.comp[0].actor = 0x11111111u;
    o.pos.comp[1].digit = 200; o.pos.comp[1].actor = 0x22222222u;
    o.tag = "cw.delta";
    const char* s = "hello world";
    o.data = (const void*)s; o.size = (size_t)strlen(s);
    o.init_blob = NULL; o.init_size = 0;
    o.init_hash = 0;
    o.prompt_edits_inc = 0;
    o.prompt_nonempty = 0;
    return o;
}

static void test_roundtrip(void){
    ConOp in = make_op_basic();
    uint8_t* buf = NULL; size_t len = 0;
    assert(conop_wire_encode(&in, &buf, &len) == 0);
    /* decode full frame */
    ConOp out; char* tag=NULL; void* data=NULL; size_t dlen=0; void* init=NULL; size_t ilen=0;
    assert(conop_wire_decode(buf, len, &out, &tag, &data, &dlen, &init, &ilen) == 0);

    assert(out.type == in.type);
    assert(out.console_id == in.console_id);
    assert(out.op_id == in.op_id);
    assert(out.actor_id == in.actor_id);
    assert(out.hlc == in.hlc);
    assert(out.user_id == in.user_id);
    assert(out.new_item_id == in.new_item_id);
    assert(out.parent_left == in.parent_left);
    assert(out.parent_right == in.parent_right);
    assert(out.pos.depth == in.pos.depth);
    assert(out.pos.comp[0].digit == in.pos.comp[0].digit);
    assert(out.pos.comp[1].actor == in.pos.comp[1].actor);
    assert(tag && strcmp(tag, "cw.delta")==0);
    assert(dlen == strlen("hello world"));
    assert(memcmp(data, "hello world", dlen)==0);
    assert(ilen == 0);

    conop_wire_free_decoded(tag, data, init);
    free(buf);
}

static void test_streaming_chunks(void){
    ConOp in = make_op_basic();
    uint8_t* buf = NULL; size_t len = 0;
    assert(conop_wire_encode(&in, &buf, &len) == 0);

    /* гоняем разными размерами куска */
    for (size_t step=1; step<=32; ++step){
        Cow1Decoder d; cow1_decoder_init(&d);
        size_t off = 0;
        int taken_total = 0;
        while (off < len){
            size_t chunk = step; if (off + chunk > len) chunk = len - off;
            cow1_decoder_consume(&d, buf+off, chunk);
            off += chunk;
            for (;;){
                ConOp out; char* tag=NULL; void* data=NULL; size_t dlen=0; void* init=NULL; size_t ilen=0;
                int k = cow1_decoder_take_next(&d, &out, &tag, &data, &dlen, &init, &ilen);
                if (k <= 0) break;
                /* проверим несколько полей */
                assert(out.type == in.type);
                assert(out.op_id == in.op_id);
                assert(tag && strcmp(tag, "cw.delta")==0);
                assert(dlen == in.size);
                conop_wire_free_decoded(tag, data, init);
                taken_total++;
            }
        }
        assert(taken_total == 1);
        cow1_decoder_reset(&d);
    }
    free(buf);
}

static void test_limits_validation(void){
    /* Сконструируем руками повреждённый буфер: верно всё, кроме tag_len > COW1_MAX_TAG */
    ConOp in = make_op_basic();
    uint8_t* buf = NULL; size_t len = 0;
    assert(conop_wire_encode(&in, &buf, &len) == 0);
    /* изменить tag_len (смещение: 4 + magic(4) + ver(2) + header до tag_len) */
    /* Воспользуемся декодером для безопасного пути: просто увеличим префикс tag_len на много */
    size_t pos_tag_len = 0;
    {
        /* просчитаем offset вручную аналогично encode() */
        size_t off = 4 /* prefix */ + 4 /* magic */ + 2 /* ver */ +
            2 /* type */ + 8 + 8 + 4 + 8 + 4 + 8 + 4 + 8 + 8 + 8 +
            (1 + (size_t)CON_POS_MAX_DEPTH*(2+4)) + 8 + 4 + 4;
        pos_tag_len = off;
    }
    /* поставим заведомо превышающее */
    uint32_t bad = (uint32_t)(COW1_MAX_TAG + 1u);
    buf[pos_tag_len+0] = (uint8_t)(bad & 0xFF);
    buf[pos_tag_len+1] = (uint8_t)((bad >> 8) & 0xFF);
    buf[pos_tag_len+2] = (uint8_t)((bad >> 16) & 0xFF);
    buf[pos_tag_len+3] = (uint8_t)((bad >> 24) & 0xFF);
    /* Попробуем декодировать — должно упасть с ошибкой */
    ConOp out; char* tag=NULL; void* data=NULL; size_t dlen=0; void* init=NULL; size_t ilen=0;
    assert(conop_wire_decode(buf, len, &out, &tag, &data, &dlen, &init, &ilen) != 0);
    free(buf);
}

int main(void){
    test_roundtrip();
    test_streaming_chunks();
    test_limits_validation();
    printf("OK: conop_wire roundtrip + streaming + limits\n");
    return 0;
}
