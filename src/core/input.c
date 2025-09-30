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

    /* если идёт активный drag для этого пользователя — обновляем позицию заранее */
    WMDrag* d = wm_get_drag(wm, uid);
    if (d && d->active) {
        wm_drag_update_pos(wm, uid, mx, my);
    }

    // клик ЛКМ — определить top и фокус
    Window *top = wm_topmost_at(wm, mx, my);
    if (e->type==3 && e->mouse.button==1 && e->mouse.state==1){
        if (top) wm_bring_to_front(wm, top);
        wm_focus_set(wm, uid, top);
    }

    Window *target = wm_focus_get(wm, uid);
    if (!target) {
        /* но dnd поверх unfocused тоже должен работать: enter/over/leave идут в top */
        if (d && d->active) target = top;
        else return;
    }

    // сохраняем старый прямоугольник окна до обработки
    Rect oldf = target->frame;

    /* ---- если активен dnd для uid, раскатываем протокол enter/over/leave ---- */
    if (d && d->active){
        Window* new_hover = top;
        if (new_hover != d->hover){
            if (d->hover && d->hover->vt && d->hover->vt->on_drag_leave){
                d->hover->vt->on_drag_leave(d->hover, d);
            }
            d->hover = new_hover;
            if (d->hover && d->hover->vt && d->hover->vt->on_drag_enter){
                d->hover->vt->on_drag_enter(d->hover, d);
            }
        }
        if (d->hover && d->hover->vt && d->hover->vt->on_drag_over){
            int lx = mx - d->hover->frame.x;
            int ly = my - d->hover->frame.y;
            d->effect = WM_DRAG_NONE; /* reset, target может выставить */
            d->hover->vt->on_drag_over(d->hover, d, lx, ly);
        }

        /* завершение dnd по отпусканию ЛКМ */
        if (e->type==3 && e->mouse.button==1 && e->mouse.state==0){
            if (d->hover && d->hover->vt && d->hover->vt->on_drop){
                int lx = mx - d->hover->frame.x;
                int ly = my - d->hover->frame.y;
                d->effect = WM_DRAG_NONE;
                d->hover->vt->on_drop(d->hover, d, lx, ly);
            }
            wm_end_drag(wm, uid);
            /* drop обработан — не пробрасываем этот button-up дальше в on_event */
            return;
        }
    }

    /* Пока активен drag, мышь уже «обслужена» протоколом DnD.
       Съедаем mouse-события, чтобы окна не реагировали вторично. */
    /* if (e->type==3 || e->type==4 || e->type==5){ */
    /*     return; */
    /* } */

    /* ---- обычная маршрутизация событий в сфокусированное окно ---- */
    if (!target) return;

    int lx = mx - target->frame.x;
    int ly = my - target->frame.y;
    if (target->vt && target->vt->on_event) {
        target->vt->on_event(target, wm, e, lx, ly);
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
    if (t && t->vt && t->vt->on_event) t->vt->on_event(t, wm, e, 0, 0);
    add_damage_if_needed(wm, t);
}

void input_route_text(WM* wm, const InputEvent *e){
    Window *t = wm_focus_get(wm, e->user_id);
    if (t && t->vt && t->vt->on_event) t->vt->on_event(t, wm, e, 0, 0);
    add_damage_if_needed(wm, t);
}
