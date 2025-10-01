#pragma once
#include "../core/window.h"
#include "console/store.h"
#include "console/processor.h"
#include "console/sink.h"

/* Вариант на одного пользователя-промпт внизу: общий Store + индивидуальный prompt_user_id */
void win_console_init(Window *w, Rect frame, int z,
                      ConsoleStore* store, ConsoleProcessor* proc, ConsoleSink* sink, int prompt_user_id);

/* Ленивый режим отображения виджетов */
typedef enum {
    LAZY_OFF = 0,             /* всегда интерактивные виджеты (как сейчас) */
    LAZY_TEXT_UNTIL_CLICK,    /* рисуем плейсхолдер строкой/плашкой, активируем по клику */
    LAZY_ALWAYS_TEXT          /* всегда строкой, активации нет */
} LazyMode;

/* Переключить режим для данной консольной вьюхи */
void win_console_set_lazy_mode(Window* w, LazyMode mode);
