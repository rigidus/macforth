#include "core/loop_hooks.h"
#include <stdlib.h>

struct LoopHookHandle {
    int priority;
    LoopHookFn fn;
    void* user;
    int alive; /* 1 — активен, 0 — к удалению */
    struct LoopHookHandle* next;
};

static struct LoopHookHandle* g_end_of_frame = NULL;

LoopHookHandle* loop_hook_add_end_of_frame(int priority, LoopHookFn fn, void* user){
    struct LoopHookHandle* h = (struct LoopHookHandle*)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->priority = priority;
    h->fn = fn;
    h->user = user;
    h->alive = 1;

    /* Вставка по приоритету (меньше — раньше) */
    if (!g_end_of_frame || priority < g_end_of_frame->priority){
        h->next = g_end_of_frame;
        g_end_of_frame = h;
        return h;
    }
    struct LoopHookHandle* cur = g_end_of_frame;
    while (cur->next && cur->next->priority <= priority) cur = cur->next;
    h->next = cur->next;
    cur->next = h;
    return h;
}

void loop_hook_remove(LoopHookHandle* h){
    if (!h) return;
    h->alive = 0; /* фактическое освобождение — после прогона */
}

void loop_hook_run_end_of_frame(uint32_t now_ms){
    /* вызов */
    for (struct LoopHookHandle* it = g_end_of_frame; it; it = it->next){
        if (it->alive && it->fn) it->fn(it->user, now_ms);
    }
    /* сборка мусора (удаляем помеченные) */
    struct LoopHookHandle* prev = NULL;
    struct LoopHookHandle* it = g_end_of_frame;
    while (it){
        if (!it->alive){
            struct LoopHookHandle* dead = it;
            if (prev) prev->next = it->next; else g_end_of_frame = it->next;
            it = it->next;
            free(dead);
        } else {
            prev = it;
            it = it->next;
        }
    }
}

void loop_hook_shutdown(void){
    struct LoopHookHandle* it = g_end_of_frame;
    while (it){
        struct LoopHookHandle* next = it->next;
        free(it);
        it = next;
    }
    g_end_of_frame = NULL;
}
