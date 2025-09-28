#pragma once
#include <stdbool.h>
#include <stdint.h>

struct WM;

typedef struct Platform Platform;

Platform* plat_create(const char *title, int w, int h);
void      plat_destroy(Platform*);
void      plat_get_output_size(Platform*, int *w, int *h);

bool      plat_poll_events_and_dispatch(Platform*, struct WM*);
void      plat_compose_and_present(Platform*, struct WM*);

uint32_t  plat_now_ms(void);
