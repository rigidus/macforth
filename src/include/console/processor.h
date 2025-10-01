#pragma once
#include "store.h"

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

#ifdef __cplusplus
}
#endif
