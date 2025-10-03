// main.c — точка входа
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "core/wm.h"
#include "core/window.h"
#include "core/drag.h"
#include "platform/platform_sdl.h"
#include "apps/win_paint.h"
#include "apps/win_square.h"
#include "apps/win_console.h"
#include "console/store.h"
#include "console/sink.h"
#include "console/processor.h"
#include "console/replicator.h"
#include "replication/type_registry.h"
#include "replication/hub.h"
#include "replication/backends/leader_tcp.h"
#include "replication/backends/local_loop.h"
#include "replication/backends/crdt_mesh.h"
#include "replication/backends/client_tcp.h"

#if defined(_WIN32) && !defined(__MINGW32__)
#  define strtok_r(s,delim,saveptr) strtok_s((s),(delim),(saveptr))
#endif

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

/* ===== Helpers: чтение чисел из окружения ===== */
static uint64_t env_u64(const char* name, uint64_t def){
    const char* s = getenv(name);
    if (!s || !*s) return def;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 0); /* base=0: поддержит 10/16 (0x...) */
    if (end == s) return def;
    return (uint64_t)v;
}
static int env_int(const char* name, int def){
    const char* s = getenv(name);
    if (!s || !*s) return def;
    char* end = NULL;
    long v = strtol(s, &end, 0); /* base=0 */
    if (end == s) return def;
    /* без отрицательных портов */
    if (v < 0) return 0;
    if (v > 65535) v = 65535;
    return (int)v;
}

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

/* === Регистрация типа «консоль» (file-scope helpers) === */


static void s_console_init_from_blob(void* user, uint32_t schema,
                                     const void* blob, size_t len){
    con_processor_init_from_blob((ConsoleProcessor*)user, schema, blob, len);
}

static void s_console_apply(void* user, const ConOp* op){
    ConsoleProcessor* p = (ConsoleProcessor*)user;
    con_processor_apply_external(p, op);
}

static int s_console_snapshot(void* user,
                              uint32_t* out_schema,
                              void** out_blob, size_t* out_len){
    return con_processor_snapshot((ConsoleProcessor*)user,
                                  out_schema, out_blob, out_len);
}

static const TypeVt s_console_type_vt = {
    .name = "console",
    .init_from_blob = s_console_init_from_blob,
    .apply = s_console_apply,
    .snapshot = s_console_snapshot,
};


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


    /* Выберем фиксированный type_id для консоли (1). */
    type_registry_register_default(/*type_id=*/1u, &s_console_type_vt, con_proc);

    /* === ReplHub (шлюз/роутер) === */
    /* Собираем доступные бэкенды и отдаём их в Hub c приоритетами. */

    /* Параметры из окружения:
       CONSOLE_ID  → console_id (u64, по умолчанию 1)
       LEADER_PORT → порт лидера (int, по умолчанию 33334; 0 — отключить)
       MESH_PORT   → порт CRDT mesh (int, по умолчанию 33335; 0 — отключить)
       MESH_SEEDS  → уже поддержан (host1,host2,...)
       CLIENT_HOST + CLIENT_PORT → авто-коннект клиента (native)
    */
    uint64_t console_id = env_u64("CONSOLE_ID", 1);
    int leader_port = env_int("LEADER_PORT", 33334);
    int mesh_port   = env_int("MESH_PORT",   33335);
    const char* client_host = getenv("CLIENT_HOST");
    int client_port  = env_int("CLIENT_PORT", 0);

    ReplBackendRef backends[4];
    int bn = 0;

    // leader/local — всегда пробуем
    Replicator* b_local  = replicator_create_local_loop();
    Replicator* b_leader = NULL;
    if (leader_port > 0){
        b_leader = replicator_create_leader_tcp(poller, console_id, (uint16_t)leader_port);
    }
    /* клиент (по умолчанию не подключаемся; целевой host/port зададим командами) */
    /* клиент: если заданы CLIENT_HOST/CLIENT_PORT — подключаемся сразу */
    Replicator* b_client = replicator_create_client_tcp(
        poller, console_id,
        (client_host && *client_host && client_port > 0) ? client_host : NULL,
        (uint16_t)client_port
        );

    if (b_leader) backends[bn++] = (ReplBackendRef){ .r=b_leader, .priority=100 };
    if (b_local)  backends[bn++] = (ReplBackendRef){ .r=b_local,  .priority=10  };
#if !defined(__EMSCRIPTEN__)
    if (b_client) backends[bn++] = (ReplBackendRef){ .r=b_client, .priority=90 };
#endif

    // mesh — только native; порт из MESH_PORT; seeds из MESH_SEEDS (host1,host2)
#if !defined(__EMSCRIPTEN__)
    const char* seeds_env = getenv("MESH_SEEDS");
    const char* seeds_arr[8]; int sn=0;
    char tmpbuf[256] = {0};
    if (seeds_env && *seeds_env){
        strncpy(tmpbuf, seeds_env, sizeof(tmpbuf)-1);
        char* save = NULL;
        for (char* tok = strtok_r(tmpbuf, ",", &save); tok && sn<8; tok = strtok_r(NULL, ",", &save)){
            seeds_arr[sn++] = tok;
        }
    }
    if (mesh_port > 0){
        Replicator* b_mesh = replicator_create_crdt_mesh(poller, (uint16_t)mesh_port, sn?seeds_arr:NULL, sn);
        if (b_mesh) backends[bn++] = (ReplBackendRef){ .r=b_mesh, .priority=50 };
    }
#endif

    /* Требования по умолчанию: без специфики, политика сама выберет LEADER>LOCAL. */
    Replicator* repl = replicator_create_hub(backends, bn, /*required_caps=*/0, /*adopt_backends=*/1);

    /* Sink: локальная спекуляция + подтверждения от репликатора */
    ConsoleSink*      con_sink  = con_sink_create(con_store, con_proc, repl, console_id, /*is_listener=*/1);
    /* Процессор публикует ответы через sink */
    con_processor_set_sink(con_proc, con_sink);

    /* Дадим процессору доступ к Hub/console_id — для рантайм-команд. */
    con_processor_set_repl_hub(con_proc, repl);
    con_processor_set_console_id(con_proc, console_id);

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
