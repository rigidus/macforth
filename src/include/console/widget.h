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
