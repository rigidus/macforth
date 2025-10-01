#pragma once
#include "../core/window.h"
#include "console/store.h"
#include "console/processor.h"
#include "console/sink.h"

/* Вариант на одного пользователя-промпт внизу: общий Store + индивидуальный prompt_user_id */
void win_console_init(Window *w, Rect frame, int z,
                      ConsoleStore* store, ConsoleProcessor* proc, ConsoleSink* sink, int prompt_user_id);
