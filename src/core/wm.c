#include "wm.h"
#include "../gfx/surface.h"
#include <stdlib.h>

static int point_in_rect(int x,int y, Rect r){
    return x>=r.x && y>=r.y && x<r.x+r.w && y<r.y+r.h;
}

WM* wm_create(int sw, int sh){
    WM *wm = (WM*)calloc(1,sizeof(WM));
    wm->screen_w = sw; wm->screen_h = sh;
    damage_init(&wm->damage);
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

void wm_resize(WM* wm, int w,int h){
    wm->screen_w=w; wm->screen_h=h;
    wm_damage_add(wm, rect_make(0,0,w,h));
    for(int i=0;i<wm->count;i++){
        Window *win=wm->win[i];
        // простая эвристика: если окно начинается в (0,0) — считаем фон и растягиваем
        if (win->frame.x==0 && win->frame.y==0){
            if (win->cache) surface_free(win->cache);
            win->frame.w=w; win->frame.h=h;
            win->cache = surface_create_argb(w,h);
            win->invalid_all = true;
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
