#pragma once
#include "../core/window.h"
#include "console/sink.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Окно с двумя вертикальными промптами (user_1=0, user_2=1). */
    void win_prompts_init(Window* w, Rect frame, int z, ConsoleSink* sink);

#ifdef __cplusplus
}
#endif
