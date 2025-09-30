#include "apps/global_state.h"
#include <stdatomic.h>

/* Простейшее глобальное состояние для демо виджета */
static _Atomic(uint32_t) g_color = 0xFF000000;

void global_set_color(uint32_t argb){
    atomic_store(&g_color, argb);
}

uint32_t global_get_color(void){
    return atomic_load(&g_color);
}
