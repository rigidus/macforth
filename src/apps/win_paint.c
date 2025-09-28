#include "win_paint.h"
#include "../gfx/surface.h"
#include "../core/wm.h"
#include "../core/drag.h"

static void draw(Window *w, const Rect *area){
    (void)area;
    w->invalid_all = false;
}

static void on_event(Window *w, void* wm, const InputEvent *e, int lx, int ly){
    if (e->type==3){ // mouse button
        if (e->mouse.button==1 && e->mouse.state==1){
            uint32_t *px = surface_pixels(w->cache);
            int pitch_px = surface_pitch(w->cache)/4;
            if ((unsigned)lx < (unsigned)surface_w(w->cache) && (unsigned)ly < (unsigned)surface_h(w->cache)){
                px[ly*pitch_px + lx] = 0xFFFFFFFF;
                w->invalid_all = true;
            }
        }
    } else if (e->type==4 && (e->mouse.buttons & 1)){ // бит0 = ЛКМ
        uint32_t *px = surface_pixels(w->cache);
        int pitch_px = surface_pitch(w->cache)/4;
        if ((unsigned)lx < (unsigned)surface_w(w->cache) && (unsigned)ly < (unsigned)surface_h(w->cache)){
            px[ly*pitch_px + lx] = 0xFFFFFFFF;
            w->invalid_all = true;
        }
    }
}

static void paint_drag_over(Window* w, WMDrag* d, int lx, int ly){
    (void)w;(void)lx;(void)ly;
    d->effect = WM_DRAG_REJECT;
}

static const WindowVTable V = {
    .draw = draw,
    .on_event = on_event,
    .tick = NULL,
    .on_focus = NULL,
    .destroy = NULL,
    .on_drag_enter = NULL,
    .on_drag_over  = paint_drag_over,
    .on_drag_leave = NULL,
    .on_drop       = NULL
};

void win_paint_init(Window *w, Rect frame, int z){
    window_init(w, "paint", frame, z, &V);
    surface_checkerboard(w->cache, 16, 0xFF181818, 0xFF101010);
}
