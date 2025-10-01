#include "console/prompt.h"
#include "gfx/text.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "apps/win_square.h"   /* SquarePayload для S-exp */
#include "console/store.h"

#ifndef CON_MAX_LINE
#define CON_MAX_LINE 4096
#endif

typedef struct ConsolePrompt {
    int          user_id;
    ConsoleSink* sink;
    ConsoleStore* store;          /* источник правды для текста промпта */
    int          cursor_col;      /* символы, не пиксели (локальная позиция) */
    /* blink */
    uint32_t   next_blink_ms;
    int        blink_on;
    /* colors */
    uint32_t   col_bg, col_fg;
} ConsolePrompt;

static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }

ConsolePrompt* con_prompt_create(int user_id, ConsoleSink* sink, ConsoleStore* store){
    ConsolePrompt* p = (ConsolePrompt*)calloc(1, sizeof(ConsolePrompt));
    if (!p) return NULL;
    p->user_id = user_id;
    p->sink    = sink;
    p->store   = store;
    p->cursor_col = 0;
    p->next_blink_ms = SDL_GetTicks() + 500;
    p->blink_on = 1;
    p->col_bg = 0xFF0A0A0A;
    p->col_fg = 0xFFFFFFFF;
    return p;
}

void con_prompt_destroy(ConsolePrompt* p){
    if (!p) return;
    free(p);
}

void con_prompt_set_colors(ConsolePrompt* p, uint32_t bg, uint32_t fg){
    if (!p) return;
    p->col_bg = bg; p->col_fg = fg;
}

static void draw_line_text(Surface *dst, int x, int y, const char *s, uint32_t argb){
    if (!s || !*s) return;
    Surface *glyph = text_render_utf8(s, argb);
    if (!glyph) return;
    surface_blit(glyph, 0,0, surface_w(glyph), surface_h(glyph), dst, x,y);
    surface_free(glyph);
}

void con_prompt_draw(ConsolePrompt* p, Surface* dst, int x, int y, int w, int h){
    if (!p || !dst) return;
    /* фон */
    surface_fill_rect(dst, x, y, w, h, p->col_bg);
    /* текст */
    int glyph_h=16, dummy_w=8;
    text_measure_utf8("M", &dummy_w, &glyph_h);
    int baseline_off = h - glyph_h;
    /* читаем текущий буфер из Store только для СВОЕГО user_id */
    char line[CON_MAX_LINE]; line[0]=0;
    if (p->store){
        (void)con_store_prompt_peek(p->store, p->user_id, line, (int)sizeof(line));
    }
    draw_line_text(dst, x, y + baseline_off, line, p->col_fg);
    /* курсор (тонкая вертикальная черта) */
    if (p->blink_on){
        /* грубая оценка ширины символа — по 'M' */
        int cw = dummy_w>0? dummy_w:8;
        int len = (int)SDL_strlen(line);
        p->cursor_col = clampi(p->cursor_col, 0, len);
        int cx = x + p->cursor_col * cw;
        surface_fill_rect(dst, cx, y, 2, h, p->col_fg);
    }
}

void con_prompt_tick(ConsolePrompt* p, uint32_t now){
    if (!p) return;
    if (now >= p->next_blink_ms){
        p->blink_on = !p->blink_on;
        p->next_blink_ms = now + 500;
    }
}

int con_prompt_get_user_id(const ConsolePrompt* p){ return p? p->user_id : -1; }

int con_prompt_get_text(char* out, int cap){
    (void)out; (void)cap;
    /* не используется снаружи прямо сейчас; можно реализовать копирование при необходимости */
    return 0;
}

void con_prompt_insert_text(ConsolePrompt* p, const char* utf8){
    if (!p || !utf8) return;
    /* локально правим буфер в Store и публикуем только метаданные (edits++, nonempty) */
    con_sink_submit_text(p->sink, p->user_id, utf8);
    /* положение курсора сдвигаем приблизительно на длину вставки */
    p->cursor_col += (int)SDL_strlen(utf8);
}

void con_prompt_backspace(ConsolePrompt* p){
    if (!p) return;
    con_sink_backspace(p->sink, p->user_id);
    if (p->cursor_col>0) p->cursor_col -= 1;
}

void con_prompt_commit(ConsolePrompt* p){
    if (!p) return;
    /* sink заберёт текст из Store и вставит в консоль (CRDT), затем очистит буфер */
    con_sink_commit(p->sink, p->user_id);
    p->cursor_col = 0;
}

int con_prompt_on_event(ConsolePrompt* p, const InputEvent* e){
    if (!p || !e) return 0;
    int changed=0;
    if (e->type == 2){ /* TEXTINPUT */
        const char* s = e->text.text;
        if (s && *s){ con_prompt_insert_text(p, s); changed=1; }
    } else if (e->type == 1){ /* KEYDOWN */
        int sym = e->key.sym;
        if (sym == SDLK_BACKSPACE){ con_prompt_backspace(p); changed=1; }
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER){ con_prompt_commit(p); changed=1; }
        else if (sym == SDLK_HOME){ p->cursor_col = 0; changed=1; }
        else if (sym == SDLK_END){
            /* длина теперь хранится в Store */
            int len = p->store ? con_store_prompt_len(p->store, p->user_id) : 0;
            p->cursor_col = len;
            changed = 1;
        }
    }
    return changed;
}

/* --- DnD helpers --- */
static void append_s_expr_square(ConsolePrompt* p, const SquarePayload* sp){
    char buf[256];
    SDL_snprintf(buf, sizeof(buf),
                 "(square :colA #x%08X :colB #x%08X :period_ms %u :phase %.3f)",
                 sp->colA, sp->colB, sp->period_ms, (double)sp->phase_bias);
    con_prompt_insert_text(p, buf);
}

void con_prompt_on_drop(ConsolePrompt* p, const char* mime, const void* data, size_t size){
    if (!p || !mime) return;
    if (strcmp(mime, "application/x-square")==0 && data && size>=sizeof(SquarePayload)){
        const SquarePayload* sp = (const SquarePayload*)data;
        append_s_expr_square(p, sp);
    } else if (strcmp(mime, "application/x-console-cmd-text")==0 && data && size>0){
        /* Вставим текстовую команду в буфер промпта */
        char tmp[CON_MAX_LINE];
        size_t n = size; if (n >= sizeof(tmp)) n = sizeof(tmp)-1;
        memcpy(tmp, data, n); tmp[n]=0;
        con_prompt_insert_text(p, tmp);
    } else {
        /* неизвестный тип — просто игнорируем */
    }
}
