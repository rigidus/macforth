#include "text.h"
#include <SDL_ttf.h>
#include <SDL.h>

static TTF_Font *g_font = NULL;

int text_init(const char *font_path, int px){
    if (TTF_Init()!=0) return -1;
    g_font = TTF_OpenFont(font_path, px);
    if(!g_font){ TTF_Quit(); return -2; }
    TTF_SetFontHinting(g_font, TTF_HINTING_LIGHT);
    return 0;
}
void text_shutdown(void){
    if (g_font){ TTF_CloseFont(g_font); g_font=NULL; }
    TTF_Quit();
}

Surface* text_render_utf8(const char *utf8, uint32_t argb){
    if (!g_font || !utf8) return NULL;
    SDL_Color col = { (argb>>16)&0xFF, (argb>>8)&0xFF, argb&0xFF, (argb>>24)&0xFF };
    SDL_Surface *s = TTF_RenderUTF8_Blended(g_font, utf8, col);
    if (!s) return NULL;
    if (s->format->format != SDL_PIXELFORMAT_ARGB8888){
        SDL_Surface *conv = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
        SDL_FreeSurface(s);
        if (!conv) return NULL;
        s = conv;
    }
    return surface_from_sdl(s);
}

int text_measure_utf8(const char *utf8, int *out_w, int *out_h){
    if (!g_font || !utf8) return -1;
    int w=0,h=0;
    if (TTF_SizeUTF8(g_font, utf8, &w, &h) != 0) return -2;
    if (out_w) *out_w = w;
    if (out_h) *out_h = TTF_FontHeight(g_font); // стабильно для строки в сетке
    return 0;
}
