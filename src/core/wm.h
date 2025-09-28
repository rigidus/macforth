#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "window.h"
#include "damage.h"

typedef struct FocusEntry {
    int user_id;
    Window *focused;
} FocusEntry;

typedef struct WM {
    Window *win[32];
    int count;

    DamageList damage;
    int screen_w, screen_h;

    FocusEntry focus[4];
} WM;

WM*  wm_create(int screen_w, int screen_h);
void wm_destroy(WM*);

void wm_add(WM*, Window*);
void wm_remove(WM*, Window*);
void wm_bring_to_front(WM*, Window*);

Window* wm_topmost_at(WM*, int x, int y);

void wm_resize(WM*, int w, int h);

bool wm_any_animating(WM*);
void wm_tick_animations(WM*, uint32_t now_ms);

void wm_damage_add(WM*, Rect r);
int  wm_damage_count(WM*);
Rect wm_damage_get(WM*, int i);

void wm_focus_set(WM*, int user_id, Window *w);
Window* wm_focus_get(WM*, int user_id);
