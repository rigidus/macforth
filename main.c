// main.c — точка входа

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
#include "console/processor.h"
#include "console/replicator.h"

#include "gfx/text.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
typedef struct { Platform* plat; WM* wm; } LoopCtx;
static LoopCtx g_ctx;

static void s_main_loop(void *p){
    LoopCtx* c = (LoopCtx*)p;

    bool running = plat_poll_events_and_dispatch(c->plat, c->wm);
    if (!running) {
        emscripten_cancel_main_loop();
        wm_destroy(c->wm);
        text_shutdown();
        plat_destroy(c->plat);
        return;
    }

    uint32_t now = plat_now_ms();
    if (wm_any_animating(c->wm)) wm_tick_animations(c->wm, now);
    plat_compose_and_present(c->plat, c->wm);
}
#endif


int main(void) {
    Platform *plat = plat_create("Cross WM", 800, 600);
    if (!plat){ fprintf(stderr,"platform init failed\n"); return 1; }

// ВАЖНО: путь к шрифту разный для native/web
    const char *font_path =
#ifdef __EMSCRIPTEN__
        "/assets/DejaVuSansMono.ttf";
#else
    "assets/DejaVuSansMono.ttf";
#endif

    if (text_init(font_path, 18) != 0){
        fprintf(stderr,"text init failed\n"); plat_destroy(plat); return 1;
    }

    int sw, sh; plat_get_output_size(plat, &sw, &sh);
    WM *wm = wm_create(sw, sh);

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

    /* единый sink для консоли (и публикует, и слушает подтверждения) */
    ConsoleSink* con_sink_for_console = con_sink_create(con_store, con_proc, repl,  console_id, 1);


    // консоль
    static Window wcon;

    /* Модель и процессор для консоли */
    win_console_init(&wcon,
                     rect_make(20, sh - sh/3 - 20, sw-40, sh/3),
                     100,
                     con_store, con_proc, con_sink_for_console);

    wm_add(wm, &wcon);

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
    }
    wm_destroy(wm);
    text_shutdown();



    /* Sink консоли */
    con_sink_destroy(con_sink_for_console);

    /* Store разделяет жизнь нескольких вьюх; освобождаем в конце программы */
    con_processor_destroy(con_proc);
    con_store_destroy(con_store);
    replicator_destroy(repl);

    plat_destroy(plat);

    return 0;
#else
    /* web-петля */
    g_ctx.plat = plat;
    g_ctx.wm   = wm;
    emscripten_set_main_loop_arg(s_main_loop, &g_ctx, 0 /*fps*/, 1 /*simulate_infinite_loop*/);
    return 0; /* сюда фактически не вернёмся */
#endif
}
