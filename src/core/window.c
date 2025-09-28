#include "window.h"
#include "../gfx/surface.h"
#include <string.h>

void window_init(Window *w, const char *name, Rect frame, int z, const WindowVTable *vt){
    memset(w,0,sizeof(*w));
    if (name){ size_t n=sizeof(w->name)-1; strncpy(w->name,name,n); w->name[n]=0; }
    w->frame = frame;
    w->zindex= z;
    w->visible = true;
    w->animating = false;
    w->invalid_all = true;
    w->vt = vt;
    w->cache = surface_create_argb(frame.w, frame.h);
    surface_fill(w->cache, 0xFF000000);
}

void window_invalidate(Window *w, Rect area_screen){
    (void)area_screen;
    w->invalid_all = true;
}
