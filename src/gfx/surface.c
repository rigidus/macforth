#include "surface.h"
#include <string.h>

Surface* surface_create_argb(int w,int h){
    Surface *wrap = (Surface*)SDL_calloc(1,sizeof(Surface));
    wrap->s = SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_ARGB8888);
    return wrap;
}
Surface* surface_from_sdl(SDL_Surface *take_ownership){
    if (!take_ownership) return NULL;
    Surface *wrap = (Surface*)SDL_calloc(1,sizeof(Surface));
    wrap->s = take_ownership;
    return wrap;
}
void surface_free(Surface* sf){ if(!sf) return; if(sf->s) SDL_FreeSurface(sf->s); SDL_free(sf); }
int  surface_w(const Surface* sf){ return sf && sf->s? sf->s->w:0; }
int  surface_h(const Surface* sf){ return sf && sf->s? sf->s->h:0; }
int  surface_pitch(const Surface* sf){ return sf && sf->s? sf->s->pitch:0; }
uint32_t* surface_pixels(Surface* sf){ return (sf && sf->s)? (uint32_t*)sf->s->pixels : NULL; }

void surface_fill(Surface* sf, uint32_t argb){
    if (!sf || !sf->s) return;
    int pitch_px = sf->s->pitch/4;
    uint32_t *base = (uint32_t*)sf->s->pixels;
    for (int y=0;y<sf->s->h;y++){
        uint32_t *row = base + y*pitch_px;
        for (int x=0;x<sf->s->w;x++) row[x]=argb;
    }
}
void surface_fill_rect(Surface* sf, int x,int y,int w,int h, uint32_t argb){
    if (!sf||!sf->s||w<=0||h<=0) return;
    int pitch_px=sf->s->pitch/4;
    int xs=x, xe=x+w; if(xs<0) xs=0; if(xe>sf->s->w) xe=sf->s->w;
    for(int yy=0; yy<h; ++yy){
        int ry=y+yy; if(ry<0||ry>=sf->s->h) continue;
        uint32_t *row = (uint32_t*)sf->s->pixels + ry*pitch_px;
        for(int rx=xs; rx<xe; ++rx) row[rx]=argb;
    }
}
void surface_blit(Surface* src, int sx,int sy,int w,int h, Surface* dst, int dx,int dy){
    if (!src||!dst||!src->s||!dst->s) return;
    SDL_Rect s = { sx,sy,w,h }, d = { dx,dy,w,h };
    SDL_BlitSurface(src->s, &s, dst->s, &d);
}
void surface_checkerboard(Surface* s, int tile, uint32_t c0, uint32_t c1){
    if (!s||!s->s||tile<=0) return;
    int pitch_px=s->s->pitch/4;
    for (int y=0;y<s->s->h;y++){
        uint32_t *row=(uint32_t*)s->s->pixels + y*pitch_px;
        int by=(y/tile)&1;
        for (int x=0;x<s->s->w;x++){
            int bx=(x/tile)&1; row[x]= (bx^by)? c0:c1;
        }
    }
    for (int y=0;y<s->s->h;y++){
        if (y%tile==0) memset((uint8_t*)s->s->pixels + y*s->s->pitch, 0x20, s->s->w*4);
    }
    for (int x=0;x<s->s->w;x++){
        if (x%tile==0){
            for (int y=0;y<s->s->h;y++){
                ((uint32_t*)s->s->pixels)[y*pitch_px + x] = 0xFF202020;
            }
        }
    }
}
