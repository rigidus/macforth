#include "win_prompts.h"
#include "gfx/surface.h"
#include "gfx/text.h"
#include "../core/wm.h"
#include "../core/drag.h"
#include "console/prompt.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>

#define MIME_CMD_TEXT "application/x-console-cmd-text"

typedef struct {
    ConsolePrompt* p_user1; /* user_id=0 */
    ConsolePrompt* p_user2; /* user_id=1 */
    ConsoleSink*   sink;    /* хранится только для жизни/симметрии */
    /* локальный фокус внутри окна: какой промпт активен для каждого user_id */
    int focused_idx[WM_MAX_USERS]; /* 0=верхний, 1=нижний, -1=нет */
    uint32_t next_blink_ms;
} PromptsView;

static void ensure_colors(ConsolePrompt* p){
    (void)p; /* пока дефолтных достаточно; хук на будущее */
}

static void draw(Window* w, const Rect* area){
    (void)area;
    PromptsView* st = (PromptsView*)w->user;
    surface_fill(w->cache, 0xFF101010);
    int total_w = surface_w(w->cache);
    int total_h = surface_h(w->cache);
    int each_h  = total_h/2;
    /* верхний */
    ensure_colors(st->p_user1);
    con_prompt_draw(st->p_user1, w->cache, 8, 8, total_w-16, each_h-12);
    /* нижний */
    ensure_colors(st->p_user2);
    con_prompt_draw(st->p_user2, w->cache, 8, each_h+4, total_w-16, each_h-12);
    w->invalid_all=false;
}

static void tick(Window* w, uint32_t now){
    PromptsView* st=(PromptsView*)w->user;
    con_prompt_tick(st->p_user1, now);
    con_prompt_tick(st->p_user2, now);
    w->invalid_all = true;
    w->next_anim_ms = now + 16;
}

static int hit_index(Window* w, int lx, int ly){
    (void)lx;
    int h = surface_h(w->cache);
    return (ly < h/2) ? 0 : 1;
}

static ConsolePrompt* idx_to_prompt(PromptsView* st, int idx){
    return (idx==0)? st->p_user1 : st->p_user2;
}

static void on_event(Window* w, void* wm_ptr, const InputEvent* e, int lx, int ly){
    (void)wm_ptr;
    PromptsView* st=(PromptsView*)w->user;
    int idx = hit_index(w, lx, ly);

    if (e->type==3 && e->mouse.button==1 && e->mouse.state==1){
        /* захват фокуса на кликнутый промпт для этого user_id */
        if (e->user_id>=0 && e->user_id<WM_MAX_USERS){
            st->focused_idx[e->user_id] = idx;
        }
        w->invalid_all = true;
        return;
    }

    /* клавиатура/текст → в промпт, который активен для данного user_id */
    int t_idx = (e->user_id>=0 && e->user_id<WM_MAX_USERS)? st->focused_idx[e->user_id] : 0;
    if (t_idx<0) t_idx = 0; /* по умолчанию — верхний */
    ConsolePrompt* tgt = idx_to_prompt(st, t_idx);
    if (tgt && (e->type==1 || e->type==2)){
        if (con_prompt_on_event(tgt, e)) w->invalid_all = true;
    }
}

/* ---- DnD ---- */
static void on_drag_over(Window* w, WMDrag* d, int lx, int ly){
    (void)w;(void)lx;(void)ly;
    if (!d || !d->mime){ d->effect = WM_DRAG_NONE; return; }
    if (strcmp(d->mime, "application/x-square")==0 ||
        strcmp(d->mime, MIME_CMD_TEXT)==0){
        d->effect = WM_DRAG_COPY;
    } else {
        d->effect = WM_DRAG_REJECT;
    }
}

static void on_drop(Window* w, WMDrag* d, int lx, int ly){
    if (!d || !d->mime) return;
    PromptsView* st=(PromptsView*)w->user;
    int idx = hit_index(w, lx, ly);
    ConsolePrompt* tgt = idx_to_prompt(st, idx);
    if (!tgt) return;
    con_prompt_on_drop(tgt, d->mime, d->data, d->size);
    d->effect = WM_DRAG_COPY;
    w->invalid_all = true;
}

static void destroy(Window* w){
    PromptsView* st=(PromptsView*)w->user;
    if (!st) return;
    if (st->p_user1) con_prompt_destroy(st->p_user1);
    if (st->p_user2) con_prompt_destroy(st->p_user2);
    /* sink не наш — его создавал внешний код; не освобождаем здесь */
    free(st); w->user=NULL;
}

static const WindowVTable V = {
    .draw = draw,
    .on_event = on_event,
    .tick = tick,
    .on_focus = NULL,
    .destroy = destroy,
    .on_frame_changed = NULL,
    .on_drag_enter = NULL,
    .on_drag_over  = on_drag_over,
    .on_drag_leave = NULL,
    .on_drop       = on_drop
};

void win_prompts_init(Window* w, Rect frame, int z, ConsoleSink* sink){
    window_init(w, "prompts", frame, z, &V);
    PromptsView* st=(PromptsView*)calloc(1, sizeof(PromptsView));
    st->sink = sink;
    st->p_user1 = con_prompt_create(0, sink);
    st->p_user2 = con_prompt_create(1, sink);
    for (int i=0;i<WM_MAX_USERS;i++) st->focused_idx[i]=0; /* по умолчанию верхний */
    w->user = st;
    w->animating = true;
    w->next_anim_ms = SDL_GetTicks();
    w->invalid_all = true;
}
