// main.c — точка входа

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "core/wm.h"
#include "core/window.h"

#include "platform/platform_sdl.h"

#include "apps/win_paint.h"
#include "apps/win_square.h"
#include "apps/win_console.h"
#include "console/store.h"
#include "console/sink.h"
#include "console/processor.h"
#include "console/replicator.h"

#include "gfx/text.h"

#include "core/loop_hooks.h"
#include "net/net.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif


/* ===== Сетевой хук: неблокирующе при активной графике, с маленьким idle-бюджетом иначе ===== */
typedef struct {
    NetPoller* poller;
    WM*        wm;
} NetHookCtx;

/* Небольшой таймаут, который не заметен для UI, но снижает пустой оборот CPU */
#ifndef NET_IDLE_BUDGET_MS
     /* держим UI отзывчивым → 0..1 мс (обычно 0) */
#   define NET_IDLE_BUDGET_MS 1
#endif


static void s_net_hook(void* user, uint32_t now_ms){
    NetHookCtx* c = (NetHookCtx*)user;
    if (!c || !c->poller) return;
    int budget = 0;
#ifndef __EMSCRIPTEN__
    /* В native можно чуть-чуть «подождать» сеть, если на экране тишина */
    if (c->wm){
        int busy = wm_any_animating(c->wm) || wm_any_drag_active(c->wm);
        if (!busy) budget = NET_IDLE_BUDGET_MS;
    }
#else
    /* в wasm всегда неблокирующе */
#endif
    net_poller_tick(c->poller, now_ms, budget);
}


#ifdef __EMSCRIPTEN__

typedef struct {
    Platform*       plat;
    WM*             wm;
    NetPoller*      poller;
    LoopHookHandle* h_net;
    /* консоль/репликация — храним тут, чтобы корректно разрушить из main loop */
    ConsoleStore*     con_store;
    ConsoleProcessor* con_proc;
    ConsoleSink*      con_sink;
    Replicator*       repl;
} LoopCtx;

static LoopCtx g_ctx;
static void s_main_loop(void *p){
    LoopCtx* c = (LoopCtx*)p;
    bool running = plat_poll_events_and_dispatch(c->plat, c->wm);
    if (!running) {
        /* порядок как в native: сперва останавливаем цикл и уничтожаем WM, затем консоль/репликация, потом поллер и платформа */
        if (c->h_net) { loop_hook_remove(c->h_net); c->h_net = NULL; }
        emscripten_cancel_main_loop();
        wm_destroy(c->wm);
        text_shutdown();
        if (c->con_proc)  { con_processor_destroy(c->con_proc); c->con_proc = NULL; }
        if (c->con_sink)  { con_sink_destroy(c->con_sink);     c->con_sink = NULL; }
        if (c->repl)      { replicator_destroy(c->repl);        c->repl = NULL; }
        if (c->con_store) { con_store_destroy(c->con_store);    c->con_store = NULL; }
        if (c->poller)    { net_poller_destroy(c->poller);      c->poller = NULL; }
        plat_destroy(c->plat);
        return;
    }
    uint32_t now = plat_now_ms();
    if (wm_any_animating(c->wm)) wm_tick_animations(c->wm, now);
    plat_compose_and_present(c->plat, c->wm);
    /* исполняем хуки конца кадра (в т.ч. сетевой поллер) */
    loop_hook_run_end_of_frame(now);
}
#endif


int main(void) {
    Platform *plat = plat_create("Cross WM", 800, 600);
    if (!plat){ fprintf(stderr,"platform init failed\n"); return 1; }

    /* NET POLLER + регистрация хука конца кадра === */
    NetPoller* poller = net_poller_create();
    if (!poller){
        fprintf(stderr,"net poller init failed\n");
        plat_destroy(plat);
        return 1;
    }
    /* приоритет 0 — по умолчанию; при необходимости можно варьировать */
    static NetHookCtx s_nethook_ctx; /* статический, чтобы жить до конца программы */
    s_nethook_ctx.poller = poller;
    s_nethook_ctx.wm      = NULL; /* заполним ниже, когда создадим WM */
    LoopHookHandle* h_net = loop_hook_add_end_of_frame(/*priority=*/0, /*fn=*/s_net_hook, /*user=*/&s_nethook_ctx);

    // FONT
    // ВАЖНО: путь к шрифту разный для native/web
    const char *font_path =
#   ifdef __EMSCRIPTEN__
        "/assets/DejaVuSansMono.ttf";
#   else
        "assets/DejaVuSansMono.ttf";
#   endif
    if (text_init(font_path, 18) != 0){
        fprintf(stderr,"text init failed\n");
        /* снять хук и уничтожить поллер, чтобы не текло */
        if (h_net) loop_hook_remove(h_net);
        if (poller) net_poller_destroy(poller);
        plat_destroy(plat);
        return 1;
    }

    int sw, sh; plat_get_output_size(plat, &sw, &sh);
    WM *wm = wm_create(sw, sh);

    /* прокинем WM в контекст сетевого хука для расчёта idle-бюджета */
    s_nethook_ctx.wm = wm;

    // фон-пэйнт
    static Window wpaint;
    win_paint_init(&wpaint, rect_make(0,0,sw,sh), 0);
    wm_add(wm, &wpaint);

    // квадраты
    static Window wq1, wq2;
    win_square_init(&wq1, rect_make((sw-360)/2, (sh-240)/2, 360,240), 1,
                    0xFFFF0000, 0xFF00FF00, 2000, 0.0f);
    wm_add(wm, &wq1);

    win_square_init(&wq2, rect_make((sw-360)/2+40, (sh-240)/2+40, 360,240), 2,
                    0xFF1E90FF, 0xFFFF66FF, 3000, 0.25f);
    wm_add(wm, &wq2);

    /* ===== Консольная модель и процессор (до создания промптов) ===== */
    ConsoleStore*     con_store = con_store_create();
    ConsoleProcessor* con_proc  = con_processor_create(con_store);
    /* Репликатор (локальный «лидер» в том же процессе) */
    Replicator*       repl      = replicator_create_crdt_local();
    /* Выделим идентификатор ленты/консоли (для одного demo — просто 1) */
    uint64_t console_id = 1;

    /* Sink: локальная спекуляция + подтверждения от репликатора */
    ConsoleSink*      con_sink  = con_sink_create(con_store, con_proc, repl, console_id, /*is_listener=*/1);
    /* Процессор публикует ответы через sink */
    con_processor_set_sink(con_proc, con_sink);

    /* Сеть - в процессор (для команд net leader/net client) */
    con_processor_set_net(con_proc, poller);

    /* Две вьюхи консоли, общее состояние, разные промпты (оба внизу) */
    static Window wcon0, wcon1;
    int con_h = sh/3;
    /* первая — снизу слева (user 0) */
    win_console_init(&wcon0, wm,
                     rect_make(20, sh - con_h - 20, (sw-60)/2, con_h),
                     100,
                     con_store, con_proc, con_sink, 0 /* user_0 */);
    wm_add(wm, &wcon0);
    /* вторая — снизу справа (user 1) */
    win_console_init(&wcon1, wm,
                     rect_make(40 + (sw-60)/2, sh - con_h - 20, (sw-60)/2, con_h),
                     101,
                     con_store, con_proc, con_sink, 1 /* user_1 */);
    wm_add(wm, &wcon1);

    /* Демонстрация: правая консоль лениво показывает виджеты строкой до клика */
    win_console_set_lazy_mode(&wcon1, LAZY_TEXT_UNTIL_CLICK);

    wm_damage_add(wm, rect_make(0,0,sw,sh));
    plat_compose_and_present(plat, wm);

#ifndef __EMSCRIPTEN__
    /* native-петля */
    bool running = true;
    while (running){
        running = plat_poll_events_and_dispatch(plat, wm);
        uint32_t now = plat_now_ms();
        if (wm_any_animating(wm)) wm_tick_animations(wm, now);
        plat_compose_and_present(plat, wm);
        /* исполняем хуки конца кадра (сеть и т.п.) */
        loop_hook_run_end_of_frame(now);
    }
    wm_destroy(wm);
    text_shutdown();

    /* DESTROYERS */
    if (h_net) loop_hook_remove(h_net);
    con_processor_destroy(con_proc);
    con_sink_destroy(con_sink);
    replicator_destroy(repl);
    con_store_destroy(con_store);
    net_poller_destroy(poller);
    plat_destroy(plat);

    return 0;
#else
    /* web-петля */
    g_ctx.plat      = plat;
    g_ctx.wm        = wm;
    g_ctx.poller    = poller;
    /* user контекст уже указывает на статический s_nethook_ctx */
    g_ctx.h_net     = h_net;
    /* сохранить объекты консоли для корректного destroy() внутри s_main_loop */
    g_ctx.con_store = con_store;
    g_ctx.con_proc  = con_proc;
    g_ctx.con_sink  = con_sink;
    g_ctx.repl      = repl;
    emscripten_set_main_loop_arg(s_main_loop, &g_ctx, 0 /*fps*/, 1 /*simulate_infinite_loop*/);
    return 0; /* сюда фактически не вернёмся */
#endif
}
