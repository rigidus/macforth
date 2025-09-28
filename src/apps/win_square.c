#include "win_square.h"
#include "../gfx/surface.h"
#include "../core/wm.h"
#include "../core/timing.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t color, colA, colB;
    uint32_t start_ms, period_ms;
    float    phase_bias;
} SquareState;

static inline uint32_t argb_lerp(uint32_t a, uint32_t b, float t){
#define CH(c,s) ((int)(((c)>>(s))&0xFF))
    int aA=CH(a,24), aR=CH(a,16), aG=CH(a,8), aB=CH(a,0);
    int bA=CH(b,24), bR=CH(b,16), bG=CH(b,8), bB=CH(b,0);
    int AA=aA+(int)((bA-aA)*t), RR=aR+(int)((bR-aR)*t), GG=aG+(int)((bG-aG)*t), BB=aB+(int)((bB-aB)*t);
    return (uint32_t)((AA<<24)|(RR<<16)|(GG<<8)|BB);
#undef CH
}

static void draw(Window *w, const Rect *area){
    (void)area;
    SquareState *st = (SquareState*)w->user;
    surface_fill(w->cache, 0xFF101010);
    int side=120;
    int x=(surface_w(w->cache)-side)/2;
    int y=(surface_h(w->cache)-side)/2;
    surface_fill_rect(w->cache, x,y, side,side, st->color);
    w->invalid_all=false;
}

static void tick(Window *w, uint32_t now){
    SquareState *st = (SquareState*)w->user;
    uint32_t period = st->period_ms? st->period_ms : 2000;
    uint32_t elapsed = now - st->start_ms;
    float u = (float)(elapsed % period)/(float)period + st->phase_bias;
    u -= floorf(u);
    float m = 0.5f - 0.5f*cosf(2.0f*3.14159265f*u);
    st->color = argb_lerp(st->colA, st->colB, m);
    w->invalid_all=true;
    w->next_anim_ms = next_frame(now);
}

static void on_event(Window *w, const InputEvent *e, int lx, int ly){
    (void)ly;
    if (e->type==3 && e->mouse.button==1 && e->mouse.state==1){
        // если клик по квадрату — развернуть фазу на полцикла
        int side=120;
        int x=(surface_w(w->cache)-side)/2;
        int y=(surface_h(w->cache)-side)/2;
        if (lx>=x && lx<x+side && ly>=y && ly<y+side){
            SquareState *st=(SquareState*)w->user;
            st->phase_bias += 0.5f; if (st->phase_bias>=1.0f) st->phase_bias-=1.0f;
            w->invalid_all=true;
        } else {
            // иначе начать drag
            w->drag.dragging=1; w->drag.dx=lx; w->drag.dy=ly;
        }
    } else if (e->type==4 && (e->mouse.buttons & SDL_BUTTON_LMASK) && w->drag.dragging){
        int nx = w->frame.x + (e->mouse.x - (w->frame.x + w->drag.dx));
        int ny = w->frame.y + (e->mouse.y - (w->frame.y + w->drag.dy));
        /* int maxx = wm->screen_w - w->frame.w; */
        /* int maxy = wm->screen_h - w->frame.h; */
        /* if (nx < 0) nx = 0; else if (nx > maxx) nx = maxx; */
        /* if (ny < 0) ny = 0; else if (ny > maxy) ny = maxy; */
        if (nx!=w->frame.x || ny!=w->frame.y){
            w->frame.x = nx;
            w->frame.y = ny;

        }
    } else if (e->type==3 && e->mouse.button==1 && e->mouse.state==0){
        w->drag.dragging=0;
    }
}

static void destroy(Window *w){
    if (w && w->user) { free(w->user); w->user=NULL; }
}

static const WindowVTable V = { draw, on_event, tick, NULL, destroy };

void win_square_init(Window *w, Rect frame, int z,
                     uint32_t colA, uint32_t colB, uint32_t period_ms, float phase){
    window_init(w, "square", frame, z, &V);
    SquareState *st = (SquareState*)malloc(sizeof(SquareState));
    memset(st,0,sizeof(*st));
    st->colA=colA; st->colB=colB; st->period_ms=period_ms; st->phase_bias=phase;
    st->start_ms = SDL_GetTicks();
    st->color = colA;
    w->user = st;
    w->animating = true;
    w->next_anim_ms = SDL_GetTicks();
}
