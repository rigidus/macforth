#include "console/sink.h"
#include <stdlib.h>
#include <string.h>
#include "console/sink.h"
#include "console/replicator.h"
#include <SDL.h>
#include "apps/widget_color.h"

#ifndef CON_SINK_PENDING_MAX
#define CON_SINK_PENDING_MAX 128
#endif

#ifndef CON_SINK_APPLIED_MAX
#define CON_SINK_APPLIED_MAX 512
#endif

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
    case CON_OP_PUT_TEXT:
        if (op->data && op->size>0){
            /* data может быть без \0 */
            char tmp[CON_MAX_LINE];
            size_t n = op->size; if (n >= sizeof(tmp)) n = sizeof(tmp)-1;
            memcpy(tmp, op->data, n); tmp[n]=0;
            con_store_put_text(s->store, tmp);
        }
        break;
    case CON_OP_BACKSPACE:
        con_store_backspace(s->store);
        break;
    case CON_OP_COMMIT:
        if (op->data && op->size>0){
            char line[CON_MAX_LINE];
            size_t n = op->size; if (n >= sizeof(line)) n = sizeof(line)-1;
            memcpy(line, op->data, n); line[n]=0;
            /* Удалённый коммит: просто добавим строку и выполним процессор */
            con_store_append_line(s->store, line);
            if (s->proc) con_processor_on_command(s->proc, line);
        } else {
            /* пустой коммит — приведём к стандартному переносу */
            con_store_commit(s->store);
        }
        break;
    case CON_OP_APPEND_LINE:
        if (op->data && op->size>0){
            char line2[CON_MAX_LINE];
            size_t n = op->size; if (n >= sizeof(line2)) n = sizeof(line2)-1;
            memcpy(line2, op->data, n); line2[n]=0;
            con_store_append_line(s->store, line2);
            if (s->proc) con_processor_on_command(s->proc, line2);
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
            con_store_insert_text_at(s->store, op->new_item_id, &op->pos, line2);
            if (s->proc) con_processor_on_command(s->proc, line2);
        }
        break;
    }
    case CON_OP_INSERT_WIDGET: {
        if (op->new_item_id != CON_ITEMID_INVALID){
            ConsoleWidget* w = NULL;
            if (op->widget_kind == 1 /* ColorSlider */){
                uint8_t init = 128;
                if (op->init_blob && op->init_size>=1) init = *(const uint8_t*)op->init_blob;
                w = widget_color_create(init);
            }
            if (w){
                con_store_insert_widget_at(s->store, op->new_item_id, &op->pos, w);
            }
        }
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
    if (repl && s->is_listener){
        replicator_set_confirm_listener(repl, s->console_id, on_confirm, s);
    }
        return s;
}

void con_sink_destroy(ConsoleSink* s){
    if (!s) return;
    free(s);
}

void con_sink_submit_text(ConsoleSink* s, int user_id, const char* utf8){
    (void)user_id;
    if (!s || !utf8) return;
    /* локальная спекуляция — только у слушателя */
    if (s->is_listener) con_store_put_text(s->store, utf8);
    /* публикация */
    if (s->repl){
        ConOp op = {0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type = CON_OP_PUT_TEXT;
        op.data = utf8; op.size = strlen(utf8);
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}

void con_sink_backspace(ConsoleSink* s, int user_id){
    (void)user_id;
    if (!s) return;
    if (s->is_listener) con_store_backspace(s->store);
    if (s->repl){
        ConOp op = {0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type = CON_OP_BACKSPACE;
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}

void con_sink_commit(ConsoleSink* s, int user_id){
    (void)user_id;
    if (!s) return;
    char cmd[CON_MAX_LINE]; int n = con_store_get_edit(s->store, cmd, sizeof(cmd));
    /* локальная спекуляция — только у слушателя */
    if (s->is_listener){
        con_store_commit(s->store);
        if (n > 0 && s->proc){ con_processor_on_command(s->proc, cmd); }
    }
    /* публикация подтверждаемой операции */
    if (s->repl){
        ConOp op = {0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type = CON_OP_COMMIT;
        op.data = (n>0)? cmd: NULL; op.size = (n>0)? (size_t)n : 0;
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}


/* ===== CRDT вставки (текст/виджеты) ===== */
void con_sink_insert_text_tail(ConsoleSink* s, int user_id, const char* utf8_line){
    if (!s || !utf8_line) return;
    ConItemId left = con_store_last_id(s->store);
    ConItemId right = CON_ITEMID_INVALID;
    ConPosId pos = con_store_gen_between(s->store, left, right, s->actor_id);
    ConItemId new_id = ((uint64_t)s->actor_id<<32) | (uint64_t)(s->next_item_seq++);
    /* локально — сразу применим у слушателя */
    if (s->is_listener){
        con_store_insert_text_at(s->store, new_id, &pos, utf8_line);
        if (s->proc) con_processor_on_command(s->proc, utf8_line);
    }
    if (s->repl){
        ConOp op = {0};
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

void con_sink_insert_widget_color(ConsoleSink* s, int user_id, uint8_t initial_r0_255){
    if (!s) return;
    ConItemId left = con_store_last_id(s->store);
    ConItemId right = CON_ITEMID_INVALID;
    ConPosId pos = con_store_gen_between(s->store, left, right, s->actor_id);
    ConItemId new_id = ((uint64_t)s->actor_id<<32) | (uint64_t)(s->next_item_seq++);
    /* локально */
    if (s->is_listener){
        ConsoleWidget* w = widget_color_create(initial_r0_255);
        if (w) con_store_insert_widget_at(s->store, new_id, &pos, w);
    }
    if (s->repl){
        ConOp op = {0};
        op.op_id   = ((uint64_t)s->actor_id<<32) | (s->next_op_id++);
        op.hlc     = con_sink_tick_hlc(s, SDL_GetTicks());
        op.actor_id= s->actor_id;
        op.console_id = s->console_id;
        op.user_id = user_id;
        op.type    = CON_OP_INSERT_WIDGET;
        op.new_item_id = new_id;
        op.parent_left = left;
        op.parent_right= right;
        op.pos = pos;
        op.widget_kind = 1; /* ColorSlider */
        uint8_t init = initial_r0_255;
        op.init_blob = &init; op.init_size = 1;
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
    (void)user_id;
    if (!s || !utf8_line) return;
    /* CRDT-вставка текста в хвост */
    con_sink_insert_text_tail(s, user_id, utf8_line);
}
