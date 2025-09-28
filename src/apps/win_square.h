#pragma once
#include "../core/window.h"

/* Переносимый payload для DnD квадрата */
typedef struct SquarePayload {
    uint32_t colA, colB;
    uint32_t period_ms;
    float    phase_bias;
} SquarePayload;

void win_square_init(Window *w, Rect frame, int z,
                     uint32_t colA, uint32_t colB, uint32_t period_ms, float phase);
