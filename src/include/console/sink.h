#pragma once
#include <stdint.h>
#include "store.h"
#include "processor.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct ConsoleSink ConsoleSink;

    /* В M1 sink — тонкая обёртка над Store (loopback, без сети) */
    /* В M2 sink знает ещё и про процессор (loopback) */
    ConsoleSink* con_sink_create(ConsoleStore* store, ConsoleProcessor* proc);
    void         con_sink_destroy(ConsoleSink* s);

    /* user_id зарезервирован под М2 (мультипользовательские промпты) */
    void con_sink_submit_text(ConsoleSink*, int user_id, const char* utf8);
    void con_sink_backspace(ConsoleSink*, int user_id);
    void con_sink_commit(ConsoleSink*, int user_id);

#ifdef __cplusplus
}
#endif
