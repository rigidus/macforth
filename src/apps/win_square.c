#include "win_square.h"
#include "../gfx/surface.h"
#include "../core/wm.h"
#include "../core/drag.h"
#include "../core/timing.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t color, colA, colB;
    uint32_t start_ms, period_ms;
    float    phase_bias;
    /* --- DnD (источник) --- */
    int drag_arm;         /* 1, если LMB внутри квадрата и ждём движения для старта DnD */
    int start_mx, start_my; /* экранные координаты нажатия */
    /* payload: что переносим между окнами квадратов */
    SquarePayload drag_payload;
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

/* маленький предпросмотр для overlay */
static Surface* make_preview(uint32_t color){
    int s=40;
    Surface* p = surface_create_argb(s, s);
    if (!p) return NULL;
    surface_fill(p, 0xAA000000);                 /* легкая тень/фон */
    surface_fill_rect(p, 2,2, s-4, s-4, color);  /* сам квадрат с рамкой */
    return p;
}


static void on_event(Window *w, void* wm_ptr, const InputEvent *e, int lx, int ly){
    (void)ly;
    WM* wm = (WM*)wm_ptr;
    SquareState *st=(SquareState*)w->user;
    int side=120;
    int x=(surface_w(w->cache)-side)/2;
    int y=(surface_h(w->cache)-side)/2;
    int inside_sq = (lx>=x && lx<x+side && ly>=y && ly<y+side);

    if (e->type==3 && e->mouse.button==1 && e->mouse.state==1){
        if (inside_sq){
            /* «армим» DnD: если пользователь начнёт тянуть — стартанём перенос */
            st->drag_arm = 1;
            st->start_mx = e->mouse.x;
            st->start_my = e->mouse.y;
        } else {
            /* drag самого окна (как раньше) */
            w->drag.dragging=1; w->drag.dx=lx; w->drag.dy=ly;
        }
    } else if (e->type==4 && (e->mouse.buttons & 1)){
        /* движение с зажатой ЛКМ */
        if (st->drag_arm){
            int dx = e->mouse.x - st->start_mx;
            int dy = e->mouse.y - st->start_my;
            if (dx*dx + dy*dy >= 9){ /* порог ~3px */
                /* стартуем DnD квадрата */
                st->drag_payload.colA = st->colA;
                st->drag_payload.colB = st->colB;
                st->drag_payload.period_ms = st->period_ms;
                st->drag_payload.phase_bias = st->phase_bias;

                Surface* prev = make_preview(st->color);
                /* горячая точка — центр превью */
                wm_start_drag(wm, e->user_id, w, "application/x-square",
                              &st->drag_payload, sizeof(st->drag_payload),
                              prev, surface_w(prev)/2, surface_h(prev)/2);
                /* примечание: prev не освободим здесь — платформа рисует overlay; освобождение в wm_end_drag */
                st->drag_arm = 0;
            }
        } else if (w->drag.dragging){
            int nx = w->frame.x + (e->mouse.x - (w->frame.x + w->drag.dx));
            int ny = w->frame.y + (e->mouse.y - (w->frame.y + w->drag.dy));
            if (nx!=w->frame.x || ny!=w->frame.y){
                /* централизованное перемещение: отметит damage старого/нового места */
                WM* wm = (WM*)wm_ptr;
                wm_window_set_frame(wm, w, rect_make(nx, ny, w->frame.w, w->frame.h));
            }
        }
    } else if (e->type==3 && e->mouse.button==1 && e->mouse.state==0){
        if (st->drag_arm){
            /* это был просто клик по квадрату, без drag: разворачиваем фазу */
            st->phase_bias += 0.5f; if (st->phase_bias>=1.0f) st->phase_bias-=1.0f;
            w->invalid_all=true;
            st->drag_arm = 0;
        }
        w->drag.dragging=0;
    }
}

static void destroy(Window *w){
    if (w && w->user) { free(w->user); w->user=NULL; }
}

/* Приёмник DnD: принимаем только "application/x-square" */
static void on_drag_enter(Window* w, const WMDrag* d){ (void)w; (void)d; }
static void on_drag_leave(Window* w, const WMDrag* d){ (void)w; (void)d; }
static void on_drag_over(Window* w, WMDrag* d, int lx, int ly){
    (void)w; (void)lx; (void)ly;
    if (d && d->mime && strcmp(d->mime, "application/x-square")==0){
        d->effect = WM_DRAG_COPY; /* показываем, что примем */
    } else {
        d->effect = WM_DRAG_NONE;
    }
}
static void on_drop(Window* w, WMDrag* d, int lx, int ly){
    (void)lx; (void)ly;
    if (!(d && d->mime && strcmp(d->mime, "application/x-square")==0)) return;
    SquareState *st = (SquareState*)w->user;
    /* перенимаем параметры источника */
    if (d->size >= sizeof(SquarePayload)){
        const SquarePayload* p = (const SquarePayload*)d->data;
        st->colA = p->colA;
        st->colB = p->colB;
        st->period_ms = p->period_ms;
        st->phase_bias = p->phase_bias;
        st->start_ms = SDL_GetTicks();
        w->invalid_all = true;
    }
    d->effect = WM_DRAG_COPY;
}

static const WindowVTable V = {
    .draw = draw,
    .on_event = on_event,
    .tick = tick,
    .on_focus = NULL,
    .destroy = destroy,
    .on_drag_enter = on_drag_enter,
    .on_drag_over  = on_drag_over,
    .on_drag_leave = on_drag_leave,
    .on_drop       = on_drop
};

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
