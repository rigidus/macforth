#include "window.h"
#include "../gfx/surface.h"
#include <string.h>
#include "wm.h"

void window_init(Window *w, const char *name, Rect frame, int z, const WindowVTable *vt){
    memset(w,0,sizeof(*w));
    if (name){ size_t n=sizeof(w->name)-1; strncpy(w->name,name,n); w->name[n]=0; }
    w->frame = frame;
    w->zindex= z;
    w->visible = true;
    w->animating = false;
    w->invalid_all = true;
    w->vt = vt;
    /* защитимся от нулевых размеров */
    int cw = frame.w > 0 ? frame.w : 1;
    int ch = frame.h > 0 ? frame.h : 1;
    w->cache = surface_create_argb(cw, ch);
    surface_fill(w->cache, 0xFF000000);
}

void window_invalidate(Window *w, Rect area_screen){
    (void)area_screen;
    w->invalid_all = true;
}

void wm_window_set_frame(WM* wm, Window* w, Rect newf){
    if (!w || !wm) return;
    Rect old = w->frame;
    /* damage старого места */
    wm_damage_add(wm, old);
    /* замена фрейма */
    w->frame = newf;
    /* пересоздать cache при изменении размера */
    int old_w = (w->cache? surface_w(w->cache):0);
    int old_h = (w->cache? surface_h(w->cache):0);
    if (newf.w != old_w || newf.h != old_h){
        if (w->cache){ surface_free(w->cache); w->cache=NULL; }
        w->cache = surface_create_argb(newf.w, newf.h);
        if (w->cache) surface_fill(w->cache, 0xFF000000);
        if (w->vt && w->vt->on_frame_changed){
            w->vt->on_frame_changed(w, old_w, old_h);
        }
        w->invalid_all = true;
    }
    /* damage нового места */
    wm_damage_add(wm, newf);
}
