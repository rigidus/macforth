#pragma once
#include "../core/window.h"
#include "console/store.h"
#include "console/processor.h"
#include "console/sink.h"

/* sink теперь передаётся извне (не создаётся внутри) */
void win_console_init(Window *w, Rect frame, int z, ConsoleStore* store, ConsoleProcessor* proc, ConsoleSink* sink);
