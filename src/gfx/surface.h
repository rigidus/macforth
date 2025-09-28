#pragma once
#include <stdint.h>
#include <SDL.h>

/* Публичное определение — внутри gfx это ок */
typedef struct Surface {
    SDL_Surface *s;  /* ARGB8888 */
} Surface;

Surface*  surface_create_argb(int w,int h);
Surface*  surface_from_sdl(SDL_Surface *take_ownership); /* принимает владение s */
void      surface_free(Surface*);
int       surface_w(const Surface*);
int       surface_h(const Surface*);
int       surface_pitch(const Surface*);
uint32_t* surface_pixels(Surface*); // ARGB8888

void surface_fill(Surface*, uint32_t argb);
void surface_fill_rect(Surface*, int x,int y,int w,int h, uint32_t argb);
void surface_blit(Surface* src, int sx,int sy,int w,int h, Surface* dst, int dx,int dy);
void surface_checkerboard(Surface*, int tile, uint32_t c0, uint32_t c1);
