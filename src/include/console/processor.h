#pragma once
#include "store.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct ConsoleProcessor ConsoleProcessor;

    ConsoleProcessor* con_processor_create(ConsoleStore* store);
    void              con_processor_destroy(ConsoleProcessor*);

    /* Вызов при подтверждении команды (Enter) */
    void con_processor_on_command(ConsoleProcessor*, const char* line_utf8);

#ifdef __cplusplus
}
#endif
