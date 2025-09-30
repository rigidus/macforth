#pragma once
#include <stdint.h>
#include <stddef.h>
#include "gfx/surface.h"
#include "core/window.h"     /* InputEvent */
#include "console/sink.h"    /* ConsoleSink */

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct ConsolePrompt ConsolePrompt;

    /* Создать/удалить промпт, связанный с конкретным пользователем и sink’ом. */
    ConsolePrompt* con_prompt_create(int user_id, ConsoleSink* sink);
    void           con_prompt_destroy(ConsolePrompt*);

    /* Настройки цветов (опционально). */
    void con_prompt_set_colors(ConsolePrompt*, uint32_t bg, uint32_t fg);

    /* Рисование прямоугольной области промпта. Высота h — высота строки. */
    void con_prompt_draw(ConsolePrompt*, Surface* dst, int x, int y, int w, int h);

    /* Тик мигания курсора (500мс внутренняя логика). now_ms — plat_now_ms(). */
    void con_prompt_tick(ConsolePrompt*, uint32_t now_ms);

    /* Обработка ввода. Возвращает 1, если нужно перерисовать. */
    int  con_prompt_on_event(ConsolePrompt*, const InputEvent* e);

    /* Явные операции редактирования — пригодятся для DnD. */
    void con_prompt_insert_text(ConsolePrompt*, const char* utf8);
    void con_prompt_backspace(ConsolePrompt*);
    void con_prompt_commit(ConsolePrompt*); /* Отправляет строку через sink->commit_text_command */

    /* Drop payload → вставить текстовое представление в буфер. */
    void con_prompt_on_drop(ConsolePrompt*, const char* mime, const void* data, size_t size);

    /* Вспомогательные геттеры (например, для хит-тестов/дебага). */
    int  con_prompt_get_user_id(const ConsolePrompt*);
    int  con_prompt_get_text(char* out, int cap);

#ifdef __cplusplus
}
#endif
