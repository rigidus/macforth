#include "console/sink.h"
#include <stdlib.h>
#include <string.h>

struct ConsoleSink {
    ConsoleStore* store;
    ConsoleProcessor* proc;
};

ConsoleSink* con_sink_create(ConsoleStore* store, ConsoleProcessor* proc){
    ConsoleSink* s = (ConsoleSink*)calloc(1, sizeof(ConsoleSink));
    if (!s) return NULL;
    s->store = store;
    s->proc  = proc;
    return s;
}

void con_sink_destroy(ConsoleSink* s){
    if (!s) return;
    free(s);
}

void con_sink_submit_text(ConsoleSink* s, int user_id, const char* utf8){
    (void)user_id; /* зарезервировано для М2 */
    if (!s) return;
    con_store_put_text(s->store, utf8);
}

void con_sink_backspace(ConsoleSink* s, int user_id){
    (void)user_id;
    if (!s) return;
    con_store_backspace(s->store);
}

void con_sink_commit(ConsoleSink* s, int user_id){
    (void)user_id;
    if (!s) return;
    /* захватываем команду до коммита */
    char cmd[CON_MAX_LINE]; int n = con_store_get_edit(s->store, cmd, sizeof(cmd));
    con_store_commit(s->store); /* команда пользователя → история */
    if (n > 0 && s->proc){
        con_processor_on_command(s->proc, cmd); /* ответы → история */
    }
}

void con_sink_widget_message(ConsoleSink* s, int user_id,
                             ConItemId id, const char* tag,
                             const void* data, size_t size){
    (void)user_id;
    if (!s) return;
    /* М4 loopback: сразу в Store → Widget */
    con_store_widget_message(s->store, id, tag, data, size);
}

void con_sink_widget_delta(ConsoleSink* s, int user_id,
                           ConItemId id, const char* tag,
                           const void* data, size_t size){
    (void)user_id;
    if (!s) return;
    /* Пока нет сети — дельта эквивалентна локальному сообщению */
    con_store_widget_message(s->store, id, tag, data, size);
}

void con_sink_commit_text_command(ConsoleSink* s, int user_id, const char* utf8_line){
    (void)user_id;
    if (!s || !utf8_line) return;
    con_store_append_line(s->store, utf8_line);
    if (s->proc) con_processor_on_command(s->proc, utf8_line);
}
