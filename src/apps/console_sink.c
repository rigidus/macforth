#include "console/sink.h"
#include <stdlib.h>
#include <string.h>
#include "console/sink.h"
#include "console/replicator.h"

#ifndef CON_SINK_PENDING_MAX
#define CON_SINK_PENDING_MAX 128
#endif

struct ConsoleSink {
    ConsoleStore* store;
    ConsoleProcessor* proc;
    Replicator* repl;
    uint64_t next_op_id;
    int is_listener;
    /* Небольшой набор ожидающих подтверждения op_id для идемпотентности */
    uint64_t pending[CON_SINK_PENDING_MAX];
    int pending_n;
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

/* Применение подтверждённой операции (если это не наша спекуляция) */
static void on_confirm(void* user, const ConOp* op){
    ConsoleSink* s = (ConsoleSink*)user;
    if (!s || !op) return;
    if (pending_has(s, op->op_id)){
        /* наше — уже применено спекулятивно */
        pending_del(s, op->op_id);
        return;
    }
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
    default: break;
    }
}

ConsoleSink* con_sink_create(ConsoleStore* store, ConsoleProcessor* proc, Replicator* repl, int is_listener){
    ConsoleSink* s = (ConsoleSink*)calloc(1, sizeof(ConsoleSink));
    if (!s) return NULL;
    s->store = store;
    s->proc  = proc;
    s->repl  = repl;
    s->next_op_id = 1;
    s->pending_n = 0;
    s->is_listener = is_listener ? 1 : 0;
    if (repl && s->is_listener){
        replicator_set_confirm_listener(repl, on_confirm, s);
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
        op.op_id = s->next_op_id++;
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
        op.op_id = s->next_op_id++;
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
        op.op_id = s->next_op_id++;
        op.user_id = user_id;
        op.type = CON_OP_COMMIT;
        op.data = (n>0)? cmd: NULL; op.size = (n>0)? (size_t)n : 0;
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
        op.op_id = s->next_op_id++;
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
        op.op_id = s->next_op_id++;
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
    /* локально — только у слушателя; промптовый sink публикует без локального дубля */
    if (s->is_listener){
        con_store_append_line(s->store, utf8_line);
        if (s->proc) con_processor_on_command(s->proc, utf8_line);
    }
    /* публикуем как APPEND_LINE */
    if (s->repl){
        ConOp op = {0};
        op.op_id = s->next_op_id++;
        op.user_id = user_id;
        op.type = CON_OP_APPEND_LINE;
        op.data = utf8_line; op.size = strlen(utf8_line);
        pending_add(s, op.op_id);
        replicator_publish(s->repl, &op);
    }
}
