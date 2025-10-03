#pragma once
#include <stdint.h>
#include <stddef.h>
#include "core/window.h"     /* InputEvent */
#include "console/sink.h"    /* ConsoleSink */
#include "gfx/surface.h"
#include "console/store.h"   /* ConsoleStore */

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct ConsolePrompt ConsolePrompt;

    /* Создать/удалить промпт, связанный с конкретным пользователем и sink’ом.
       Вся строка ввода теперь хранится в ConsoleStore (а не во View). */
    ConsolePrompt* con_prompt_create(int user_id, ConsoleSink* sink, ConsoleStore* store);
    void           con_prompt_destroy(ConsolePrompt*);

    /* Настройки цветов (опционально). */
    void con_prompt_set_colors(ConsolePrompt*, uint32_t bg, uint32_t fg);

    /* Рисование прямоугольной области промпта. Высота h — высота строки. */
    void con_prompt_draw(ConsolePrompt*, Surface* dst, int x, int y, int w, int h);

    /* Тик мигания курсора (500мс внутренняя логика). now_ms — plat_now_ms(). */
    void con_prompt_tick(ConsolePrompt*, uint32_t now_ms);

    /* Обработка ввода. Возвращает 1, если нужно перерисовать. */
    int  con_prompt_on_event(ConsolePrompt*, const InputEvent* e);

    /* Явные операции редактирования — проксируют в Store через Sink (c репликацией индикатора). */
    void con_prompt_insert_text(ConsolePrompt*, const char* utf8);
    void con_prompt_backspace(ConsolePrompt*);
    void con_prompt_commit(ConsolePrompt*); /* Берёт текст из Store и коммитит через Sink */

    /* Drop payload → вставить текстовое представление в буфер. */
    void con_prompt_on_drop(ConsolePrompt*, const char* mime, const void* data, size_t size);

    /* Вспомогательные геттеры (например, для хит-тестов/дебага). */
    int  con_prompt_get_user_id(const ConsolePrompt*);
    int  con_prompt_get_text(char* out, int cap); /* (оставлено для совместимости; вернёт 0) */

#ifdef __cplusplus
}
#endif
