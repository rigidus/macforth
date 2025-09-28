#include "platform_sdl.h"
#include <SDL.h>
#include "../core/wm.h"
#include "../core/input.h"
#include "../core/timing.h"
#include "../gfx/surface.h"
#include "../core/drag.h"

struct Platform {
    SDL_Window  *win;
    SDL_Surface *screen;   // window surface
    Surface     *back;     // ARGB backbuffer we composite into
    uint32_t     last_present_ms;
};

uint32_t plat_now_ms(void){ return SDL_GetTicks(); }

static void blit_rect_from_to(Surface *src, SDL_Surface *dst, int sx,int sy,int w,int h, int dx,int dy){
    SDL_Rect s = { sx,sy,w,h }, d = { dx,dy,w,h };
    SDL_BlitSurface(src->s, &s, dst, &d);
}

Platform* plat_create(const char *title, int w, int h){
    if (SDL_Init(SDL_INIT_VIDEO)!=0) return NULL;
    SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       w,h, SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
    if (!win) { SDL_Quit(); return NULL; }
    SDL_StartTextInput();
    Platform *pf = (Platform*)SDL_calloc(1,sizeof(Platform));
    pf->win = win;
    pf->screen = SDL_GetWindowSurface(win);
    pf->back   = surface_create_argb(pf->screen->w, pf->screen->h);
    surface_fill(pf->back, 0xFF000000);
    return pf;
}

void plat_destroy(Platform* pf){
    if (!pf) return;
    SDL_StopTextInput();
    if (pf->back) surface_free(pf->back);
    if (pf->win) SDL_DestroyWindow(pf->win);
    SDL_Quit();
    SDL_free(pf);
}

void plat_get_output_size(Platform* pf, int *w, int *h){
    if (!pf || !pf->screen){ if(w)*w=0; if(h)*h=0; return; }
    if (w) *w = pf->screen->w;
    if (h) *h = pf->screen->h;
}

bool plat_poll_events_and_dispatch(Platform* pf, WM* wm){
    (void)pf;
    SDL_Event e;
    while (SDL_PollEvent(&e)){
        if (e.type==SDL_QUIT) return false;

        InputEvent ie={0}; ie.user_id=0;
        switch (e.type){
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_ESCAPE) return false;
            ie.type=1; ie.key.sym=(int)e.key.keysym.sym; ie.key.repeat=e.key.repeat;
            input_route_key(wm, &ie);
            break;

        case SDL_TEXTINPUT:
            ie.type=2; SDL_strlcpy(ie.text.text, e.text.text, sizeof(ie.text.text));
            input_route_text(wm, &ie);
            break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            ie.type=3;
            ie.mouse.x = (e.type==SDL_MOUSEBUTTONDOWN||e.type==SDL_MOUSEBUTTONUP) ? e.button.x : 0;
            ie.mouse.y = (e.type==SDL_MOUSEBUTTONDOWN||e.type==SDL_MOUSEBUTTONUP) ? e.button.y : 0;
            ie.mouse.button = (int)e.button.button; /* реальные коды SDL_BUTTON_* */
            ie.mouse.state  = (e.type==SDL_MOUSEBUTTONDOWN) ? 1 : 0;
            input_route_mouse(wm, &ie);
            break;

        case SDL_MOUSEMOTION:
            ie.type=4;
            ie.mouse.x = e.motion.x; ie.mouse.y = e.motion.y;
            ie.mouse.dx = e.motion.xrel; ie.mouse.dy = e.motion.yrel;
            ie.mouse.buttons = (e.motion.state & SDL_BUTTON_LMASK)?1:0; // бит0 = ЛКМ
            input_route_mouse(wm, &ie);
            break;

        case SDL_MOUSEWHEEL:
            ie.type=5; ie.mouse.wheel_y = e.wheel.y;
            input_route_mouse(wm, &ie);
            break;

        case SDL_WINDOWEVENT:
            if (e.window.event==SDL_WINDOWEVENT_SIZE_CHANGED){
                pf->screen = SDL_GetWindowSurface(pf->win);
                if (pf->back) surface_free(pf->back);
                pf->back = surface_create_argb(pf->screen->w, pf->screen->h);
                surface_fill(pf->back, 0xFF000000);
                wm_resize(wm, pf->screen->w, pf->screen->h);
                /* сразу пометим damage полного экрана на side окна */
                wm_damage_add(wm, rect_make(0,0,pf->screen->w,pf->screen->h));
            } else if (e.window.event==SDL_WINDOWEVENT_EXPOSED){
                wm_damage_add(wm, rect_make(0,0, pf->screen->w, pf->screen->h));
            }
            break;
        }
    }
    return true;
}


void plat_compose_and_present(Platform* pf, WM* wm){
    int n = wm_damage_count(wm);
    bool anim = wm_any_animating(wm) || wm_any_drag_active(wm); /* dnd требует редрав без damage */
    
    if (n==0 && !anim) return;

    uint32_t t = plat_now_ms();
    if (anim && (t - pf->last_present_ms) < FRAME_MS){
        SDL_Delay(FRAME_MS - (t - pf->last_present_ms));
    }

    // Если damage нет, но нужна анимация — рисуем весь экран
    Rect full = rect_make(0,0, pf->screen->w, pf->screen->h);

    for (int di=0; di < (n? n:1); ++di){
        Rect dr = n ? wm_damage_get(wm, di) : full;

        // очистка фона в backbuffer
        surface_fill_rect(pf->back, dr.x, dr.y, dr.w, dr.h, 0xFF000000);

        // окна снизу-вверх
        for (int wi=0; wi<wm->count; ++wi){
            Window *w = wm->win[wi]; if (!w->visible) continue;
            Rect inter = rect_intersect(dr, w->frame);
            if (rect_is_empty(inter)) continue;

            // перерисовка окна при необходимости
            if (w->invalid_all && w->vt && w->vt->draw){
                w->vt->draw(w, &w->frame);
            }

            // источник в локальных координатах окна
            int sx = inter.x - w->frame.x;
            int sy = inter.y - w->frame.y;
            // blit: окно -> backbuffer
            SDL_Rect s = { sx, sy, inter.w, inter.h };
            SDL_Rect d = { inter.x, inter.y, inter.w, inter.h };
            SDL_BlitSurface(w->cache->s, &s, pf->back->s, &d);
        }

        // ВАЖНО: скопировать готовый регион из backbuffer в surface окна
        SDL_Rect r = { dr.x, dr.y, dr.w, dr.h };

        /* ----- поверх окон дорисовываем активные drag overlay для всех пользователей ----- */
        for (int uid=0; uid<WM_MAX_USERS; ++uid){
            WMDrag* d = wm_get_drag(wm, uid);
            if (!d || !d->active || !d->preview) continue;
            int ox = d->x - d->hot_x;
            int oy = d->y - d->hot_y;
            Rect ovr = rect_make(ox, oy, surface_w(d->preview), surface_h(d->preview));
            Rect inter = rect_intersect(dr, ovr);
            if (rect_is_empty(inter)) continue;
            int sx = inter.x - ox;
            int sy = inter.y - oy;
            blit_rect_from_to(d->preview, pf->back->s, sx,sy, inter.w, inter.h, inter.x, inter.y);

            /* Бейдж запрета: если текущий hover выставил REJECT/ NONE */
            if (d->effect==WM_DRAG_REJECT || d->effect==WM_DRAG_NONE){
                /* рисуем простой «no» знак 16x16 в правом-нижнем углу превью */
                int bw=16, bh=16;
                int bx = ox + surface_w(d->preview) - bw;
                int by = oy + surface_h(d->preview) - bh;
                /* круг — грубо прямоугольник с «скруглением» не делаем, просто фон и диагональ */
                surface_fill_rect(pf->back, bx, by, bw, bh, 0xCCAA0000);      /* красный фон с альфой */
                surface_fill_rect(pf->back, bx+1, by+1, bw-2, bh-2, 0xFFFF0000); /* ярче внутри */
                /* диагональная полоса */
                for (int i=0;i<bh;i++){
                    int rx = bx + i/2; /* примитивная диагональ */
                    surface_fill_rect(pf->back, rx, by+i, 8, 1, 0xFFFFFFFF);
                }
            }
        }
        
        SDL_BlitSurface(pf->back->s, &r, pf->screen, &r);
    }

    // Показать
    if (n){
        SDL_Rect rs[64]; if (n>64) n=64;
        for (int i=0;i<n;i++){ Rect r=wm_damage_get(wm,i); rs[i]=(SDL_Rect){r.x,r.y,r.w,r.h}; }
        SDL_UpdateWindowSurfaceRects(pf->win, rs, n);
    } else {
        // полный экран при анимации
        SDL_UpdateWindowSurface(pf->win);
    }

    pf->last_present_ms = plat_now_ms();
    damage_clear(&wm->damage);
}
