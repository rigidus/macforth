#include "apps/widget_color.h"
#include "gfx/text.h"
#include "apps/global_state.h"
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

typedef struct {
    ConsoleWidget base;
    int value;     /* 0..255 (красный канал) */
    int dragging;  /* 0/1 */
} ColorSlider;

static const char* color_as_text(ConsoleWidget* self, char* out, int cap);

static int clampi(int v, int lo, int hi){ return v<lo?lo:(v>hi?hi:v); }

static void color_draw(ConsoleWidget* self, Surface* dst, int x, int y, int w, int h, uint32_t fg){
    (void)fg;
    ColorSlider* cs = (ColorSlider*)self;
    /* фон ячейки */
    surface_fill_rect(dst, x, y, w, h, 0xFF101010);
    /* полоса с «интенсивностью» красного */
    int bar_h = h/3;
    int bar_y = y + (h - bar_h)/2;
    surface_fill_rect(dst, x, bar_y, w, bar_h, 0xFF202020);

    /* заполнение по значению */
    int fill_w = (int)((cs->value / 255.0f) * w);
    if (fill_w > 0){
        uint32_t col = (uint32_t)(0xFF000000 | (cs->value<<16));
        surface_fill_rect(dst, x, bar_y, fill_w, bar_h, col);
    }
    /* «ползунок» */
    int knob_x = x + (int)((cs->value / 255.0f) * w);
    surface_fill_rect(dst, knob_x-1, bar_y-2, 3, bar_h+4, 0xFFFFFFFF);

    /* подпись */
    char label[32]; SDL_snprintf(label, sizeof(label), "R=%d", cs->value);
    Surface* lab = text_render_utf8(label, 0xFFFFFFFF);
    if (lab){
        surface_blit(lab, 0,0, surface_w(lab), surface_h(lab), dst, x+4, y+4);
        surface_free(lab);
    }
}

/* адресные сообщения
   tag: "set" (payload: int 0..255), "delta" (payload: int d) */
static int color_on_message(ConsoleWidget* self, const char* tag, const void* data, size_t size){
    ColorSlider* cs = (ColorSlider*)self;
    if (!tag) return 0;
    int old = cs->value;
    if (strcmp(tag, "set")==0 && data && size>=sizeof(int)){
        int v = *(const int*)data;
        cs->value = clampi(v, 0, 255);
    } else if (strcmp(tag, "delta")==0 && data && size>=sizeof(int)){
        int d = *(const int*)data;
        cs->value = clampi(cs->value + d, 0, 255);
    } else {
        return 0;
    }
    if (cs->value != old){
        uint32_t argb = 0xFF000000 | ((uint32_t)cs->value<<16);
        global_set_color(argb);
        return 1;
    }
    return 0;
}

static int pos_to_value(int lx, int w){
    if (w<=0) return 0;
    int v = (int)( (lx / (float)w) * 255.0f + 0.5f );
    return clampi(v, 0, 255);
}

static int color_on_event(ConsoleWidget* self, const InputEvent* e, int lx, int ly, int w, int h){
    (void)ly; (void)h;
    ColorSlider* cs = (ColorSlider*)self;
    int changed = 0;
    if (e->type==3){ /* mouse button */
        if (e->mouse.button==1 && e->mouse.state==1){
            cs->dragging = 1;
            int nv = pos_to_value(lx, w);
            if (nv != cs->value){ cs->value = nv; changed = 1; }
        } else if (e->mouse.button==1 && e->mouse.state==0){
            cs->dragging = 0;
        }
    } else if (e->type==4){ /* mouse motion */
        if (cs->dragging && (e->mouse.buttons & 1)){
            int nv = pos_to_value(lx, w);
            if (nv != cs->value){ cs->value = nv; changed = 1; }
        }
    }
    if (changed){
        /* обновим глобальную переменную как демонстрацию эффекта */
        uint32_t argb = 0xFF000000 | ((uint32_t)cs->value<<16);
        global_set_color(argb);
    }
    return changed;
}

static void color_destroy(ConsoleWidget* self){
    if (!self) return;
    free(self);
}

static const char* color_as_text(ConsoleWidget* self, char* out, int cap){
    ColorSlider* cs = (ColorSlider*)self;
    if (!out || cap<8) return NULL;
    SDL_snprintf(out, cap, "[Color R=%d]", cs->value);
    return out;
}

ConsoleWidget* widget_color_create(uint8_t initial_r0_255){
    ColorSlider* cs = (ColorSlider*)calloc(1, sizeof(ColorSlider));
    if (!cs) return NULL;
    cs->value = initial_r0_255;
    cs->base.draw     = color_draw;
    cs->base.on_event = color_on_event;
    cs->base.on_message = color_on_message;
    cs->base.as_text  = color_as_text;
    cs->base.destroy  = color_destroy;
    return (ConsoleWidget*)cs;
}
