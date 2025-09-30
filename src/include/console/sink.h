#pragma once
#include <stdint.h>
#include <stddef.h>
#include "store.h"
#include "processor.h"
#include "replicator.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct ConsoleSink ConsoleSink;

    /* M8: sink делает локальную спекуляцию и публикует операции через Replicator.
       is_listener != 0 — этот sink регистрируется как получатель «подтверждённых» op’ов. */
    ConsoleSink* con_sink_create(ConsoleStore* store, ConsoleProcessor* proc, Replicator* repl, int is_listener);
    void         con_sink_destroy(ConsoleSink* s);

    /* user_id зарезервирован под М2 (мультипользовательские промпты) */
    void con_sink_submit_text(ConsoleSink*, int user_id, const char* utf8);
    void con_sink_backspace(ConsoleSink*, int user_id);
    void con_sink_commit(ConsoleSink*, int user_id);
    /* адресуемые сообщения к виджетам */
    void con_sink_widget_message(ConsoleSink*, int user_id,
                                 ConItemId id,
                                 const char* tag,
                                 const void* data, size_t size);
    /* Эмиссия «дельт» (операций), проходящая через Sink/репликатор.
       В текущем loopback-режиме эквивалентна widget_message. */
    void con_sink_widget_delta(ConsoleSink*, int user_id,
                               ConItemId id,
                               const char* tag,
                               const void* data, size_t size);
    /* Инъекция готовой командной строки: добавить в историю как TEXT и отработать процессором. */
    void con_sink_commit_text_command(ConsoleSink*, int user_id, const char* utf8_line);

    /* ---- HLC/actor helpers (для штамповки дельт виджетов) ---- */
    /* Уникальный идентификатор актора (узла) этого sink’а. */
    uint32_t con_sink_get_actor_id(ConsoleSink*);
    /* Тик локального HLC: вернёт монотонно-неубывающий «время-логический» штамп. */
    uint64_t con_sink_tick_hlc(ConsoleSink*, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
