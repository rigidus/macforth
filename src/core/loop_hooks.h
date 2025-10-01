#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct LoopHookHandle LoopHookHandle;
    typedef void (*LoopHookFn)(void* user, uint32_t now_ms);

    /* Добавить хук конца кадра. Чем меньше priority — тем раньше выполняется. */
    LoopHookHandle* loop_hook_add_end_of_frame(int priority, LoopHookFn fn, void* user);

    /* Ленивая отписка (удаляется/освобождается после ближайшего прогона). */
    void loop_hook_remove(LoopHookHandle* h);

    /* Выполнить все хуки конца кадра (вызывать из основного цикла). */
    void loop_hook_run_end_of_frame(uint32_t now_ms);

    /* Полная очистка списка (опционально). */
    void loop_hook_shutdown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
