#include "wm.h"
#include "../gfx/surface.h"
#include <stdlib.h>
#include <string.h>

static int point_in_rect(int x,int y, Rect r){
    return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;
}

WM* wm_create(int sw, int sh){
    WM *wm = (WM*)calloc(1,sizeof(WM));
    wm->screen_w = sw; wm->screen_h = sh;
    damage_init(&wm->damage);
    /* drag-сессии пустые */
    memset(wm->drag, 0, sizeof(wm->drag));
    return wm;
}
void wm_destroy(WM* wm){
    if (!wm) return;
    for(int i=0;i<wm->count;i++){
        Window *w = wm->win[i];
        if (w){
            if (w->vt && w->vt->destroy) w->vt->destroy(w);
            if (w->cache) surface_free(w->cache);
        }
    }
    free(wm);
}

static void sort_by_z(WM* wm){
    for(int i=0;i<wm->count;i++) for(int j=i+1;j<wm->count;j++){
            if (wm->win[i]->zindex > wm->win[j]->zindex){
                Window* t = wm->win[i]; wm->win[i]=wm->win[j]; wm->win[j]=t;
            }
        }
}

void wm_add(WM* wm, Window* w){
    if (wm->count>= (int)(sizeof(wm->win)/sizeof(wm->win[0]))) return;
    wm->win[wm->count++] = w;
    sort_by_z(wm);
}
void wm_remove(WM* wm, Window* w){
    for(int i=0;i<wm->count;i++) if (wm->win[i]==w){
            for(int j=i+1;j<wm->count;j++) wm->win[j-1]=wm->win[j];
            wm->count--; break;
        }
}

static int max_z(WM* wm){
    int mz = wm->count? wm->win[0]->zindex : 0;
    for(int i=1;i<wm->count;i++) if (wm->win[i]->zindex>mz) mz=wm->win[i]->zindex;
    return mz;
}
void wm_bring_to_front(WM* wm, Window* w){
    if (!w) return;
    int newZ = max_z(wm)+1;
    if (w->zindex != newZ){ w->zindex=newZ; sort_by_z(wm); }
    wm_damage_add(wm, w->frame);
    w->invalid_all = true;
}

Window* wm_topmost_at(WM* wm, int x,int y){
    Window *best=NULL; int bestZ=-2147483647;
    for(int i=0;i<wm->count;i++){
        Window *w = wm->win[i]; if(!w->visible) continue;
        if (point_in_rect(x,y,w->frame) && w->zindex>=bestZ){ best=w; bestZ=w->zindex; }
    }
    return best;
}

void wm_resize(WM* wm, int newW, int newH){
    int oldW = wm->screen_w, oldH = wm->screen_h;
    wm->screen_w = newW; wm->screen_h = newH;
    /* damage всего экрана */
    wm_damage_add(wm, rect_make(0,0,newW,newH));
    /* эвристика «фон»: окно, равное предыдущему экрану и привязанное к (0,0), растягиваем */
    for (int i=0;i<wm->count;i++){
        Window* win = wm->win[i];
        if (win->frame.x==0 && win->frame.y==0 &&
            win->frame.w==oldW && win->frame.h==oldH){
            wm_window_set_frame(wm, win, rect_make(0,0,newW,newH));
        } else {
            /* Кадр мог «выпасть» за новые границы — подожмём внутрь экрана */
            Rect nf = win->frame;
            if (nf.x + nf.w > newW) nf.x = (newW - nf.w < 0)? 0 : (newW - nf.w);
            if (nf.y + nf.h > newH) nf.y = (newH - nf.h < 0)? 0 : (newH - nf.h);
            if (nf.x < 0) nf.x = 0;
            if (nf.y < 0) nf.y = 0;
            if (nf.x!=win->frame.x || nf.y!=win->frame.y){
                wm_window_set_frame(wm, win, nf);
            } else {
                /* размер без изменений — но перерисовать после resize полезно */
                win->invalid_all = true;
                wm_damage_add(wm, win->frame);
            }
        }
    }
}

bool wm_any_animating(WM* wm){
    for(int i=0;i<wm->count;i++){
        Window* w=wm->win[i];
        if (w->visible && w->animating) return true;
    }
    return false;
}
void wm_tick_animations(WM* wm, uint32_t now){
    for(int i=0;i<wm->count;i++){
        Window* w=wm->win[i];
        if (!w->visible || !w->animating) continue;
        if (w->vt && w->vt->tick){
            if (now >= w->next_anim_ms) w->vt->tick(w, now);
        }
    }
}

void wm_damage_add(WM* wm, Rect r){ damage_add(&wm->damage, r); }
int  wm_damage_count(WM* wm){ return damage_count(&wm->damage); }
Rect wm_damage_get(WM* wm, int i){ return damage_at(&wm->damage,i); }

void wm_focus_set(WM* wm, int uid, Window *w){
    if (uid<0 || uid>= (int)(sizeof(wm->focus)/sizeof(wm->focus[0]))) return;
    Window *old = wm->focus[uid].focused;
    if (old==w) return;
    if (old && old->vt && old->vt->on_focus) old->vt->on_focus(old,false);
    wm->focus[uid].user_id=uid; wm->focus[uid].focused=w;
    if (w && w->vt && w->vt->on_focus) w->vt->on_focus(w,true);
}
Window* wm_focus_get(WM* wm, int uid){
    if (uid<0 || uid>= (int)(sizeof(wm->focus)/sizeof(wm->focus[0]))) return NULL;
    return wm->focus[uid].focused;
}

/* ---- Drag&Drop ---- */
static int valid_uid(int uid){ return uid>=0 && uid<WM_MAX_USERS; }

WMDrag* wm_get_drag(WM* wm, int user_id){
    if (!valid_uid(user_id)) return NULL;
    return &wm->drag[user_id];
}

void wm_start_drag(WM* wm, int user_id, Window* source, const char* mime,
                   void* data, size_t size, Surface* preview, int hot_x, int hot_y){
    if (!valid_uid(user_id)) return;
    WMDrag* d = &wm->drag[user_id];
    d->active = true;
    d->user_id = user_id;
    d->source = source;
    d->mime = mime;
    d->data = data;
    d->size = size;
    d->preview = preview;
    d->hot_x = hot_x; d->hot_y = hot_y;
    d->hover = NULL;
    d->effect = WM_DRAG_NONE;
    d->px = d->x; d->py = d->y;
}

void wm_drag_update_pos(WM* wm, int user_id, int x, int y){
    WMDrag* d = wm_get_drag(wm, user_id);
    if (!d || !d->active) return;
    /* Пометим damage для старого и нового положения overlay */
    int ow = (d->preview ? surface_w(d->preview) : 24);
    int oh = (d->preview ? surface_h(d->preview) : 24);
    int oox = d->px - (d->preview ? d->hot_x : ow/2);
    int ooy = d->py - (d->preview ? d->hot_y : oh/2);
    wm_damage_add(wm, rect_make(oox, ooy, ow, oh));

    d->px = d->x; d->py = d->y;
    d->x = x; d->y = y;

    int nx = d->x - (d->preview ? d->hot_x : ow/2);
    int ny = d->y - (d->preview ? d->hot_y : oh/2);
    wm_damage_add(wm, rect_make(nx, ny, ow, oh));
}

void wm_end_drag(WM* wm, int user_id){
    WMDrag* d = wm_get_drag(wm, user_id);
    if (!d) return;
    /* уведомить текущий hover о leave, если было */
    if (d->active && d->hover && d->hover->vt && d->hover->vt->on_drag_leave){
        d->hover->vt->on_drag_leave(d->hover, d);
    }
    memset(d, 0, sizeof(*d));
}

bool wm_any_drag_active(WM* wm){
    for (int i=0;i<WM_MAX_USERS;i++){
        if (wm->drag[i].active) return true;
    }
    return false;
}
