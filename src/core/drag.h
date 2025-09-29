#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct Window;
struct Surface;

/* Результат/эффект dnd, который заполняет target при over/drop */
typedef enum {
    WM_DRAG_NONE = 0,   /* не принимаю */
    WM_DRAG_COPY = 1,   /* скопировать */
    WM_DRAG_MOVE = 2,   /* переместить */
    WM_DRAG_LINK = 3,   /* ссылка/референс */
    WM_DRAG_REJECT = 4  /* явный отказ (для UI) */
} WMDragEffect;

/* Сессия drag одного актора (user_id) */
typedef struct WMDrag {
    bool     active;
    int      user_id;
    struct Window* source;
    const char* mime;    /* тип полезной нагрузки (MIME или свой тег) */
    void*    data;       /* указатель на данные (жизненный цикл на стороне source) */
    size_t   size;

    int x, y;            /* экранные координаты курсора */
    int px, py;          /* предыдущие координаты — для damage старого overlay */

    /* overlay предпросмотра */
    struct Surface* preview; /* необязательный ARGB overlay */
    int hot_x, hot_y;        /* "горячая" точка внутри preview */

    /* текущий целевой window под курсором (для enter/leave) */
    struct Window* hover;

    /* эффект, который сообщает приёмник в over/drop */
    WMDragEffect effect;
} WMDrag;
