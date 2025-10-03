#pragma once
#include "store.h"
#include "net/net.h"
#include <stdbool.h>
#include <stddef.h>  /* size_t */


/* Не тянем детали ConOp здесь: достаточно forward-declare. */
struct ConOp;

struct Replicator;
typedef struct Replicator Replicator;

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct ConsoleProcessor ConsoleProcessor;

    /* forward declare, чтобы избежать циклических include */
    typedef struct ConsoleSink ConsoleSink;

    ConsoleProcessor* con_processor_create(ConsoleStore* store);
    void              con_processor_destroy(ConsoleProcessor*);

    /* Вызов при подтверждении команды (Enter) */
    void con_processor_on_command(ConsoleProcessor*, const char* line_utf8);

    /* дать процессору доступ к Sink для публикации ответов/виджетов. */
    void con_processor_set_sink(ConsoleProcessor*, ConsoleSink* sink);

    /* дать процессору доступ к сетевому поллеру */
    void con_processor_set_net(ConsoleProcessor*, NetPoller* np);

    /* Дать процессору доступ к Hub (для рантайм-команд) и его console_id. */
    void       con_processor_set_repl_hub(ConsoleProcessor*, Replicator* hub);
    Replicator* con_processor_get_repl_hub(ConsoleProcessor*);
    void       con_processor_set_console_id(ConsoleProcessor*, uint64_t console_id);
    uint64_t   con_processor_get_console_id(ConsoleProcessor*);
    /* Нужен sink для ответов из ext-команд. */
    ConsoleSink* con_processor_get_sink(ConsoleProcessor*);

    /* Контракт как у TypeVt:
       - apply_external: применить подтверждённую операцию;
       - snapshot: сериализовать текущее состояние → (schema, blob),
                   вернуть 0 при успехе; *out_blob* выделяет компонент (free() снаружи);
       - init_from_blob: инициализация состояния из снапшота указанной версии schema. */
    void con_processor_apply_external(ConsoleProcessor*, const struct ConOp* op);
    int  con_processor_snapshot(ConsoleProcessor* self,
                                uint32_t* out_schema, void** out_blob, size_t* out_len);
    void con_processor_init_from_blob(ConsoleProcessor*,
                                      uint32_t schema, const void* blob, size_t len);

    /* Хук: расширенные команды. Возвращает 1, если команда обработана. */
    int  con_processor_ext_try_handle(ConsoleProcessor*, const char* line_utf8);

#ifdef __cplusplus
}
#endif
