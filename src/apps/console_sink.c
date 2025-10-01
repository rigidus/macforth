#include <stdlib.h>
#include <string.h>
#include "console/sink.h"
#include "console/replicator.h"
#include <SDL.h>
#include "apps/widget_color.h"
#include <stdint.h>
#include <stdio.h>

#ifndef CON_SINK_PENDING_MAX
#define CON_SINK_PENDING_MAX 128
#endif

#ifndef CON_SINK_APPLIED_MAX
#define CON_SINK_APPLIED_MAX 512
#endif

typedef struct {
    uint64_t h;
    void*    data;
    size_t   size;
} BlobEnt;

static uint64_t fnv1a64(const void* p, size_t n){
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull; /* offset basis */
    for (size_t i=0;i<n;i++){ h ^= b[i]; h *= 1099511628211ull; }
    return h;
}


#ifndef CON_SINK_BLOB_MAX
#define CON_SINK_BLOB_MAX 64
#endif

typedef struct {
    BlobEnt arr[CON_SINK_BLOB_MAX];
    int     n;
} BlobCache;

static void blob_put_local(BlobCache* bc, uint64_t h, const void* data, size_t size){
    if (!bc || !h || !data || !size) return;
    int idx = (bc->n < CON_SINK_BLOB_MAX) ? bc->n++ : (bc->n-1);
    if (bc->n >= CON_SINK_BLOB_MAX) { /* LRU-наивно: сдвиг */
        memmove(&bc->arr[0], &bc->arr[1], (CON_SINK_BLOB_MAX-1)*sizeof(BlobEnt));
        idx = CON_SINK_BLOB_MAX-1;
    }
    bc->arr[idx].h = h;
    bc->arr[idx].data = malloc(size);
    if (bc->arr[idx].data){ memcpy(bc->arr[idx].data, data, size); bc->arr[idx].size = size; }
}

static const BlobEnt* blob_get_local(const BlobCache* bc, uint64_t h){
    if (!bc || !h) return NULL;
    for (int i=0;i<bc->n;i++) if (bc->arr[i].h == h) return &bc->arr[i];
    return NULL;
}

struct ConsoleSink {
    ConsoleStore* store;
    ConsoleProcessor* proc;
    Replicator* repl;
    uint64_t  console_id;
    uint64_t next_op_id;
    uint32_t actor_id;
    uint64_t last_hlc;
    uint32_t next_item_seq;
    int is_listener;
    /* Небольшой набор ожидающих подтверждения op_id для идемпотентности */
    uint64_t pending[CON_SINK_PENDING_MAX];
    int pending_n;
    /* Идемпотентность на уровне приёмника: уже применённые op_id */
    uint64_t applied[CON_SINK_APPLIED_MAX];
    int applied_n;
    /* Локальный кэш init_blob по контент-хэшу (пер-инстанс) */
    BlobCache blobs;
};

static int pending_has(ConsoleSink* s, uint64_t id){
    for (int i=0;i<s->pending_n;i++) if (s->pending[i]==id) return 1;
    return 0;
}

static void pending_add(ConsoleSink* s, uint64_t id){
    if (s->pending_n < CON_SINK_PENDING_MAX) s->pending[s->pending_n++] = id;
    else { /* простая политика: вытолкнуть самый старый */
        memmove(s->pending, s->pending+1, (CON_SINK_PENDING_MAX-1)*sizeof(uint64_t));
        s->pending[CON_SINK_PENDING_MAX-1] = id;
    }
}

static void pending_del(ConsoleSink* s, uint64_t id){
    for (int i=0;i<s->pending_n;i++){
        if (s->pending[i]==id){
            for (int j=i+1;j<s->pending_n;j++) s->pending[j-1]=s->pending[j];
            s->pending_n--; return;
        }
    }
}

static int applied_has(ConsoleSink* s, uint64_t id){
    for (int i=0;i<s->applied_n;i++) if (s->applied[i]==id) return 1;
    return 0;
}
static void applied_add(ConsoleSink* s, uint64_t id){
    if (s->applied_n < CON_SINK_APPLIED_MAX) s->applied[s->applied_n++] = id;
    else {
        memmove(s->applied, s->applied+1, (CON_SINK_APPLIED_MAX-1)*sizeof(uint64_t));
        s->applied[CON_SINK_APPLIED_MAX-1] = id;
    }
}

/* ---- HLC helpers ---- */
uint32_t con_sink_get_actor_id(ConsoleSink* s){ return s ? s->actor_id : 0; }
uint64_t con_sink_tick_hlc(ConsoleSink* s, uint64_t now_ms){
    if (!s) return 0;
    /* простой гибрид: физическое время, склеенное с логическим хвостом (одно число).
       Гарантируем монотонность: если now_ms <= last_hlc, двигаем логический компонент (+1). */
    uint64_t now = now_ms;
    uint64_t last = s->last_hlc;
    uint64_t next = (now > last) ? now : (last + 1);
    s->last_hlc = next;
    return next;
}


/* Применение подтверждённой операции (если это не наша спекуляция) */
static void on_confirm(void* user, const ConOp* op){
    ConsoleSink* s = (ConsoleSink*)user;
    if (!s || !op) return;
    /* Доп. защита: фильтруем не свою консоль (на случай ошибочного роутинга) */
    if (op->console_id != s->console_id) return;
    /* дубликаты от сети (или эхо) — игнорировать по op_id */
    if (applied_has(s, op->op_id)){
        return;
    }
    if (pending_has(s, op->op_id)){
        /* наше — уже применено спекулятивно */
        pending_del(s, op->op_id);
        applied_add(s, op->op_id);
        return;
    }
    applied_add(s, op->op_id);
    /* чужое — применяем к Store идемпотентно по типу */
    switch (op->type){
    case CON_OP_APPEND_LINE:
        if (op->data && op->size>0){
            char line2[CON_MAX_LINE];
            size_t n = op->size; if (n >= sizeof(line2)) n = sizeof(line2)-1;
            memcpy(line2, op->data, n); line2[n]=0;
            /* это «выход» процессора — только отрисовать, НЕ вызывать процессор повторно */
            con_store_append_line(s->store, line2);
        }
        break;
    case CON_OP_WIDGET_MSG:
    case CON_OP_WIDGET_DELTA:
        if (op->widget_id != CON_ITEMID_INVALID){
            con_store_widget_message(s->store, op->widget_id, op->tag?op->tag:"", op->data, op->size);
        }
        break;
    case CON_OP_INSERT_TEXT: {
        if (op->new_item_id != CON_ITEMID_INVALID && op->data && op->size>0){
            char line2[CON_MAX_LINE];
            size_t n = op->size; if (n >= sizeof(line2)) n = sizeof(line2)-1;
            memcpy(line2, op->data, n); line2[n]=0;
            con_store_insert_text_at(s->store, op->new_item_id, &op->pos, line2, op->user_id);
            if (s->proc) con_processor_on_command(s->proc, line2);
        }
        break;
    }
    case CON_OP_INSERT_WIDGET: {
        if (op->new_item_id != CON_ITEMID_INVALID){
            ConsoleWidget* w = NULL;
            if (op->widget_kind == 1 /* ColorSlider */){
                uint8_t init = 128;
                if (op->init_blob && op->init_size>=1){
                    init = *(const uint8_t*)op->init_blob;
                } else if (op->init_hash){
                    /* попробуем достать init из ЛОКАЛЬНОГО blob-кэша sink’а */
                    const BlobEnt* be = blob_get_local(&s->blobs, op->init_hash);
                    if (be && be->size>=1) init = *(const uint8_t*)be->data;
                }
                w = widget_color_create(init);
            }
            if (w){
                con_store_insert_widget_at(s->store, op->new_item_id, &op->pos, w, op->user_id);

            }
        }
        break;
    }
    case CON_OP_PROMPT_META: {
        /* применяем ТОЛЬКО индикатор (edits++, nonempty), без текста */
        con_store_prompt_apply_meta(s->store, op->user_id, op->prompt_edits_inc, op->prompt_nonempty);
        break;
    }
    default: break;
    }
}

ConsoleSink* con_sink_create(ConsoleStore* store, ConsoleProcessor* proc, Replicator* repl, uint64_t console_id, int is_listener){
    ConsoleSink* s = (ConsoleSink*)calloc(1, sizeof(ConsoleSink));
    if (!s) return NULL;
    s->store = store;
    s->proc  = proc;
    s->repl  = repl;
    s->console_id = console_id;
    s->next_op_id = 1;
    /* простой actor_id: смесь адреса и стартового времени */
    s->actor_id = (uint32_t)((uintptr_t)s ^ (uintptr_t)SDL_GetTicks());
    s->last_hlc = SDL_GetTicks();
    s->next_item_seq = 1;
    s->pending_n = 0;
    s->applied_n = 0;
    s->is_listener = is_listener ? 1 : 0;
    memset(&s->blobs, 0, sizeof(s->blobs));
    if (repl && s->is_listener){
        replicator_set_confirm_listener(repl, s->console_id, on_confirm, s);
    }
        return s;
}

void con_sink_destroy(ConsoleSink* s){
    if (!s) return;
    free(s);
}

/* ===== операции промпта — локальная правка + рассылка индикатора ===== */
static void publish_prompt_meta(ConsoleSink* s, int user_id){
    if (!s || !s->repl) return;
    int nonempty = con_store_prompt_len(s->store, user_id) > 0 ? 1 : 0;
    ConOp op = (ConOp){0};
    op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
    op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
    op.actor_id= s->actor_id;
    op.console_id = s->console_id;
    op.user_id = user_id;
    op.type    = CON_OP_PROMPT_META;
    op.prompt_edits_inc = 1;
    op.prompt_nonempty  = nonempty;
    pending_add(s, op.op_id);
    replicator_publish(s->repl, &op);
}

void con_sink_submit_text(ConsoleSink* s, int user_id, const char* utf8){
    if (!s || !utf8) return;
    /* локально меняем буфер промпта */
    con_store_prompt_insert(s->store, user_id, utf8, /*bump=*/1);
    /* и шлём только метаданные */
    publish_prompt_meta(s, user_id);
}

void con_sink_backspace(ConsoleSink* s, int user_id){
    if (!s) return;
    con_store_prompt_backspace(s->store, user_id, /*bump=*/1);
    publish_prompt_meta(s, user_id);
}

void con_sink_commit(ConsoleSink* s, int user_id){
    if (!s) return;
    char line[CON_MAX_LINE];
    int n = con_store_prompt_take(s->store, user_id, line, (int)sizeof(line));
    /* после очистки буфера — обновим индикатор (nonempty=0) */
    publish_prompt_meta(s, user_id);
    if (n>0){
        /* добавить как команду (CRDT-вставка текста в хвост + выполнить процессором) */
        con_sink_commit_text_command(s, user_id, line);
    }
}

/* ===== публикация готовой строки (выход процессора) ===== */
void con_sink_append_line(ConsoleSink* s, int user_id, const char* utf8_line){
    if (!s || !utf8_line) return;
    /* локальная спекуляция: просто добавить строку без вызова процессора */
    if (s->is_listener){
        con_store_append_line(s->store, utf8_line);
    }
    if (s->repl){
        ConOp op = {0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type    = CON_OP_APPEND_LINE;
        op.data = utf8_line; op.size = strlen(utf8_line);
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}

/* ===== вставки в произвольные якоря (between) ===== */
void con_sink_insert_text_between(ConsoleSink* s, int user_id, ConItemId left, ConItemId right, const char* utf8_line){
    if (!s || !utf8_line) return;
    ConPosId pos = con_store_gen_between(s->store, left, right, s->actor_id);
    ConItemId new_id = ((uint64_t)s->actor_id<<32) | (uint64_t)(s->next_item_seq++);
    if (s->is_listener){
        con_store_insert_text_at(s->store, new_id, &pos, utf8_line, user_id);
        if (s->proc) con_processor_on_command(s->proc, utf8_line);
    }
    if (s->repl){
        ConOp op = (ConOp){0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type    = CON_OP_INSERT_TEXT;
        op.new_item_id = new_id;
        op.parent_left = left;
        op.parent_right= right;
        op.pos = pos;
        op.data = utf8_line; op.size = strlen(utf8_line);
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}

/* ===== CRDT вставки (текст/виджеты) ===== */
void con_sink_insert_text_tail(ConsoleSink* s, int user_id, const char* utf8_line){
    if (!s || !utf8_line) return;
    ConItemId left = con_store_last_id(s->store);
    ConItemId right = CON_ITEMID_INVALID;
    con_sink_insert_text_between(s, user_id, left, right, utf8_line);
}

void con_sink_insert_widget_color(ConsoleSink* s, int user_id, uint8_t initial_r0_255){
    if (!s) return;

    /* всегда вставляем в хвост */
    ConItemId left  = con_store_last_id(s->store);
    ConItemId right = CON_ITEMID_INVALID;
    ConPosId  pos   = con_store_gen_between(s->store, left, right, s->actor_id);
    ConItemId new_id = ((uint64_t)s->actor_id<<32) | (uint64_t)(s->next_item_seq++);

    /* локальная вставка для слушателя */
    if (s->is_listener){
        ConsoleWidget* w = widget_color_create(initial_r0_255);
        if (w) {
            con_store_insert_widget_at(s->store, new_id, &pos, w, user_id);
        }
    }

    /* репликация операции */
    if (s->repl){
        ConOp op = (ConOp){0};
        op.op_id       = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc         = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id    = s->actor_id;
        op.console_id  = s->console_id;
        op.user_id     = user_id;
        op.type        = CON_OP_INSERT_WIDGET;
        op.new_item_id = new_id;
        op.parent_left = left;
        op.parent_right= right;
        op.pos         = pos;
        op.widget_kind = 1; /* ColorSlider */
        uint8_t init   = initial_r0_255;
        op.init_blob = &init; op.init_size = 1;
        op.init_hash = fnv1a64(&init, 1);
        /* класть в локальный кэш инстанса sink’а */
        blob_put_local(&s->blobs, op.init_hash, &init, 1);
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}

void con_sink_widget_message(ConsoleSink* s, int user_id,
                             ConItemId id, const char* tag,
                             const void* data, size_t size){
    (void)user_id;
    if (!s) return;
    /* локальная спекуляция — только у слушателя */
    if (s->is_listener) con_store_widget_message(s->store, id, tag, data, size);
    /* и публикация */
    if (s->repl){
        ConOp op = {0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type = CON_OP_WIDGET_MSG;
        op.widget_id = id;
        op.tag = tag;
        op.data = data; op.size = size;
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}

void con_sink_widget_delta(ConsoleSink* s, int user_id,
                           ConItemId id, const char* tag,
                           const void* data, size_t size){
    (void)user_id;
    if (!s) return;
    /* локальная спекуляция — только у слушателя */
    if (s->is_listener) con_store_widget_message(s->store, id, tag, data, size);
    /* публикация */
    if (s->repl){
        ConOp op = {0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type = CON_OP_WIDGET_DELTA;
        op.widget_id = id;
        op.tag = tag;
        op.data = data; op.size = size;
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}

void con_sink_commit_text_command(ConsoleSink* s, int user_id, const char* utf8_line){
    if (!s || !utf8_line) return;
    /* CRDT-вставка текста в хвост */
    con_sink_insert_text_tail(s, user_id, utf8_line);
}
