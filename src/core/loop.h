// Мини-интеграция в main:
// LoopHooks* hooks = loop_create();
// NetPoller* np = net_poller_create();
// loop_add_hook(hooks, s_net_hook, np, "net", 0);
// while (running) {
// plat_poll_events_and_dispatch(...);
// loop_run_hooks(hooks, plat_now_ms());
// render_frame();
// }
// loop_remove_hook(hooks, s_net_hook, np);
// net_poller_destroy(np);
// loop_destroy(hooks);
//
// В вебе (Emscripten) соберётся без сети (заглушки).
// На POSIX — poll(); на Windows — WSAPoll().
// =============================================================


// === file: src/core/loop.h ===
#pragma once
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


    typedef struct LoopHooks LoopHooks;
    typedef void (*LoopHookFn)(void* user, uint32_t now_ms);


    LoopHooks* loop_create(void);
    void loop_destroy(LoopHooks*);


// priority: ниже — раньше; выше — позже; 0 — по умолчанию
    int loop_add_hook(LoopHooks*, LoopHookFn fn, void* user, const char* name, int priority);
    void loop_remove_hook(LoopHooks*, LoopHookFn fn, void* user);


// Вызывайте каждый кадр
    void loop_run_hooks(LoopHooks*, uint32_t now_ms);


#ifdef __cplusplus
}
#endif
