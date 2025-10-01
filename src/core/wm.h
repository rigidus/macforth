#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "window.h"
#include "damage.h"
#include "drag.h"

/* Максимум одновременных пользователей (акторов) */
#ifndef WM_MAX_USERS
#define WM_MAX_USERS 8
#endif

typedef struct FocusEntry {
    int user_id;
    Window *focused;
} FocusEntry;

typedef struct WM {
    Window *win[32];
    int count;

    DamageList damage;
    int screen_w, screen_h;

    FocusEntry focus[WM_MAX_USERS];

    /* drag-and-drop сессии: по одной на user_id */
    WMDrag drag[WM_MAX_USERS];
} WM;

WM*  wm_create(int screen_w, int screen_h);
void wm_destroy(WM*);

void wm_add(WM*, Window*);
void wm_remove(WM*, Window*);
void wm_bring_to_front(WM*, Window*);

Window* wm_topmost_at(WM*, int x, int y);

void wm_resize(WM* wm, int newW, int newH);

bool wm_any_animating(WM*);
void wm_tick_animations(WM*, uint32_t now_ms);

void wm_damage_add(WM*, Rect r);
int  wm_damage_count(WM*);
Rect wm_damage_get(WM*, int i);

void wm_focus_set(WM*, int user_id, Window *w);
Window* wm_focus_get(WM*, int user_id);

/* ---- Drag & Drop API ---- */
/* Начать сессию drag от имени user_id. preview может быть NULL. mime/data/size — по договору между окнами. */
void wm_start_drag(WM* wm, int user_id, Window* source, const char* mime,
                   void* data, size_t size, Surface* preview, int hot_x, int hot_y);
/* Обновить позицию курсора пользователя (обычно из mouse motion) */
void wm_drag_update_pos(WM* wm, int user_id, int x, int y);
/* Принудительно завершить dnd (после drop или отмены) */
void wm_end_drag(WM* wm, int user_id);
/* Доступ к текущей сессии для обработчиков */
WMDrag* wm_get_drag(WM* wm, int user_id);

/* Есть ли хотя бы одна активная drag-сессия? */
bool wm_any_drag_active(WM* wm);

/* Безопасно изменить позицию/размер окна: пересоздаёт cache при изменении размера,
   грязнит старый и новый прямоугольники, вызывает on_frame_changed */
void wm_window_set_frame(WM* wm, Window* w, Rect newf);

/* Инвалидация окна с немедленным добавлением damage (area_screen — в экранных координатах).
   Если прямоугольник пустой, грязнится весь frame окна. */
void wm_window_invalidate(WM* wm, Window* w, Rect area_screen);
