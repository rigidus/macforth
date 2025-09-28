#include "input.h"
#include "wm.h"

static void add_damage_if_needed(WM* wm, Window *w){
    if (w && w->invalid_all){
        wm_damage_add(wm, w->frame);
    }
}

void input_route_mouse(WM* wm, const InputEvent *e){
    int uid = e->user_id;
    int mx = e->mouse.x, my = e->mouse.y;

    // клик ЛКМ — определить top и фокус
    Window *top = wm_topmost_at(wm, mx, my);
    if (e->type==3 && e->mouse.button==1 && e->mouse.state==1){
        if (top) wm_bring_to_front(wm, top);
        wm_focus_set(wm, uid, top);
    }

    Window *target = wm_focus_get(wm, uid);
    if (!target) return;

    // сохраняем старый прямоугольник окна до обработки
    Rect oldf = target->frame;

    int lx = mx - target->frame.x;
    int ly = my - target->frame.y;
    if (target->vt && target->vt->on_event) {
        target->vt->on_event(target, e, lx, ly);
    }

    // если окно изменило положение/размер — грязним и старую, и новую области
    if (oldf.x != target->frame.x || oldf.y != target->frame.y ||
        oldf.w != target->frame.w || oldf.h != target->frame.h) {
        wm_damage_add(wm, oldf);
        wm_damage_add(wm, target->frame);
        target->invalid_all = true; // перерисовать кэш окна в новом месте
    }

    add_damage_if_needed(wm, target);
}

void input_route_key(WM* wm, const InputEvent *e){
    Window *t = wm_focus_get(wm, e->user_id);
    if (t && t->vt && t->vt->on_event) t->vt->on_event(t, e, 0, 0);
    add_damage_if_needed(wm, t);
}

void input_route_text(WM* wm, const InputEvent *e){
    Window *t = wm_focus_get(wm, e->user_id);
    if (t && t->vt && t->vt->on_event) t->vt->on_event(t, e, 0, 0);
    add_damage_if_needed(wm, t);
}
