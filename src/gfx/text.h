#pragma once
#include <stdint.h>
#include "surface.h"

int  text_init(const char *font_path, int px);
void text_shutdown(void);

/* Рендер строки в ARGB Surface (caller: surface_free) */
Surface* text_render_utf8(const char *utf8, uint32_t argb);

/* Измерение строки: возвращает 0 при успехе */
int text_measure_utf8(const char *utf8, int *out_w, int *out_h);
