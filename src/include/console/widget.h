#pragma once
#include "gfx/surface.h"
#include "core/window.h"   /* InputEvent */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct ConsoleWidget ConsoleWidget;

    struct ConsoleWidget {
        /* Отрисовка в прямоугольник ячейки (x,y,w,h) */
        void (*draw)(ConsoleWidget* self, Surface* dst, int x, int y, int w, int h, uint32_t fg);
        /* Обработка входа; возвращает 1, если состояние изменилось (нужно перерисовать/уведомить) */
        int  (*on_event)(ConsoleWidget* self, const InputEvent* e, int lx, int ly, int w, int h);
        /* Адресуемые сообщения (для сетевых/скриптовых обновлений).
           tag — произвольная команда, data/size — полезная нагрузка. Возвращает 1, если состояние изменилось. */
        int  (*on_message)(ConsoleWidget* self, const char* tag, const void* data, size_t size);
        /* Текстовая форма для «ленивых»/строковых отображений (может вернуть внутренний/временный буфер или out). */
        const char* (*as_text)(ConsoleWidget* self, char* out, int cap);
        /* Снимок текущего состояния виджета в виде бинарного blob — для эмиссии «дельт» (set_state).
           На входе *inout_size — размер буфера out; на выходе — фактически записанный размер.
           Возвращает 1 при успехе, 0 — если сериализация не поддерживается. */
        int  (*get_state_blob)(ConsoleWidget* self, void* out, size_t* inout_size);

        /* Освобождение */
        void (*destroy)(ConsoleWidget* self);
    };

    /* Удобный helper — дерегирует на destroy, если есть */
    static inline void con_widget_destroy(ConsoleWidget* w){
        if (w && w->destroy) w->destroy(w);
    }

#ifdef __cplusplus
}
#endif
