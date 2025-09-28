#include "win_console.h"
#include "../gfx/surface.h"
#include "../gfx/text.h"
#include "../core/drag.h"
#include "../core/timing.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define CON_MAX_LINE   4096
#define CON_BUF_LINES  1024

/* C11-переносимая замена strdup (POSIX) */
static char* xstrdup(const char* s){
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

typedef struct {
    /* сетка и метрики */
    int cell_w, cell_h;
    int cols, rows;

    /* история (кольцевой буфер) */
    char *lines[CON_BUF_LINES];
    int   line_len[CON_BUF_LINES];
    int   head;   /* индекс самой старой */
    int   count;  /* актуальное кол-во */

    /* редактируемая строка */
    char  edit[CON_MAX_LINE];
    int   edit_len;
    int   cursor_col;

    /* курсор/мигание */
    bool  blink_on;
    uint32_t next_blink_ms;

    int   glyph_h;   /* высота строки для выравнивания по базовой линии */

    /* цвета */
    uint32_t col_bg;
    uint32_t col_fg;
} ConsoleState;

/* ---------- utils ---------- */

static void console_free_lines(ConsoleState *st){
    for (int i=0;i<CON_BUF_LINES;i++){
        if (st->lines[i]) { free(st->lines[i]); st->lines[i]=NULL; }
        st->line_len[i]=0;
    }
    st->head=0; st->count=0;
}

static void console_measure(ConsoleState *st, int win_w, int win_h){
    int wM=0, hM=0;
    if (text_measure_utf8("M", &wM, &hM) != 0){ wM=8; hM=16; }
    st->cell_w = (wM>0? wM:8);
    st->cell_h = (hM>0? hM:16);
    /* для выравнивания текста к низу ячейки используем высоту строки шрифта */
    st->glyph_h = st->cell_h;

    st->cols = win_w / st->cell_w;  if (st->cols<1) st->cols=1;
    st->rows = win_h / st->cell_h;  if (st->rows<2) st->rows=2; // минимум: 1 история + 1 edit
}

/* хук окна: пересчитать метрики сетки при смене размера */
static void console_on_frame_changed(Window* w, int old_w, int old_h){
    (void)old_w; (void)old_h;
    ConsoleState *st = (ConsoleState*)w->user;
    console_measure(st, surface_w(w->cache), surface_h(w->cache));
    /* ограничим курсор по новым колонкам */
    if (st->cursor_col > st->cols) st->cursor_col = st->cols;
    w->invalid_all = true;
}

static void console_dirty_line(Window* w, ConsoleState* st, int vis_row){
    if (vis_row<0 || vis_row>=st->rows) return;
    Rect r = rect_make(w->frame.x, w->frame.y + vis_row*st->cell_h,
                       st->cols*st->cell_w, st->cell_h);
    window_invalidate(w, r);
}

/* сохранить edit в историю */
static void console_commit_line(ConsoleState* st){
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free(st->lines[idx]); st->lines[idx]=NULL;
    st->lines[idx] = (char*)malloc((size_t)st->edit_len + 1);
    if (st->lines[idx]){
        memcpy(st->lines[idx], st->edit, (size_t)st->edit_len);
        st->lines[idx][st->edit_len]=0;
        st->line_len[idx] = st->edit_len;
        if (st->count < CON_BUF_LINES) st->count++;
        else st->head = (st->head + 1) % CON_BUF_LINES;
    }
    st->edit_len=0; st->edit[0]=0; st->cursor_col=0;
}

/* вставка текста в edit с жёстким переносом по колонкам */
static void console_put_text(ConsoleState* st, const char *utf8){
    while (*utf8){
        unsigned char c = (unsigned char)*utf8++;
        if (st->edit_len < CON_MAX_LINE-1){
            st->edit[st->edit_len++] = (char)c;
            st->edit[st->edit_len] = 0;
            st->cursor_col++;
            if (st->cursor_col >= st->cols){ /* wrap */
                console_commit_line(st);
            }
        }
    }
}

/* ---------- рисунок ---------- */

static void draw_line_text(Surface *dst, int x, int y, const char *s, uint32_t argb){
    if (!s || !*s) return;
    Surface *glyph = text_render_utf8(s, argb);
    if (!glyph) return;
    surface_blit(glyph, 0,0, surface_w(glyph), surface_h(glyph), dst, x,y);
    surface_free(glyph);
}

static void console_draw(Window *w, const Rect *area){
    (void)area;
    ConsoleState *st = (ConsoleState*)w->user;
    int baseline_off = st->cell_h - st->glyph_h;
    
    /* очистить окно */
    surface_fill(w->cache, st->col_bg);

    /* сколько видимых строк истории? */
    int history_rows = st->rows - 1;
    if (history_rows < 0) history_rows = 0;

    /* берём последние history_rows из st->count */
    int start = st->count - history_rows;
    if (start < 0) start = 0;

    int vis_row = 0;

    /* рисуем историю */
    for (; vis_row < history_rows; ++vis_row){
        int line_idx = start + vis_row;
        if (line_idx < st->count){
            int idx = (st->head + line_idx) % CON_BUF_LINES;
            const char *s = st->lines[idx] ? st->lines[idx] : "";
            draw_line_text(w->cache, 0, vis_row * st->cell_h + baseline_off, s, st->col_fg);
        }
    }

    /* рисуем редактируемую строку (последняя) */
    int edit_y = vis_row * st->cell_h;
    if (st->edit_len){
        draw_line_text(w->cache, 0, edit_y + baseline_off, st->edit, st->col_fg);   
    }
    /* курсор */
    if (st->blink_on){
        int cx = st->cursor_col * st->cell_w;
        surface_fill_rect(w->cache, cx, edit_y, 2, st->cell_h, st->col_fg);
    }

    w->invalid_all = false;
}

/* --- destroy: освобождаем буферы истории --- */
static void console_destroy(Window *w){
    if (!w) return;
    ConsoleState *st = (ConsoleState*)w->user;
    if (st){
        console_free_lines(st);
        free(st);
    }
    w->user = NULL;
}

/* ---------- тик анимации (мигание курсора) ---------- */

static void console_tick(Window *w, uint32_t now){
    ConsoleState *st = (ConsoleState*)w->user;
    if (now >= st->next_blink_ms){
        st->blink_on = !st->blink_on;
        st->next_blink_ms = now + 500;
        /* грязним только строку курсора */
        console_dirty_line(w, st, st->rows - 1);
    }
    w->next_anim_ms = next_frame(now);
}

/* ---------- ввод ---------- */

static void console_on_event(Window *w, void* wm, const InputEvent *e, int lx, int ly){
    (void)lx; (void)ly;
    ConsoleState *st = (ConsoleState*)w->user;

    if (e->type == 2){ /* TEXTINPUT */
        console_put_text(st, e->text.text);
        console_dirty_line(w, st, st->rows-1);
        w->invalid_all = true;
    } else if (e->type == 1){ /* KEYDOWN */
        int sym = e->key.sym;
        if (sym == SDLK_BACKSPACE){
            if (st->edit_len > 0){
                st->edit[--st->edit_len] = 0;
                if (st->cursor_col>0) st->cursor_col--;
                console_dirty_line(w, st, st->rows-1);
                w->invalid_all = true;
            }
        } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER){
            console_commit_line(st);
            /* перерисовать весь видимый блок (история сдвинулась) */
            w->invalid_all = true;
        } else if (sym == SDLK_HOME){
            st->cursor_col = 0; console_dirty_line(w, st, st->rows-1); w->invalid_all = true;
        } else if (sym == SDLK_END){
            st->cursor_col = st->edit_len;
            if (st->cursor_col > st->cols) st->cursor_col = st->cols;
            console_dirty_line(w, st, st->rows-1); w->invalid_all = true;
        }
    }
}

/* ---------- vtable и init ---------- */

/* Пример: при удержании ЛКМ начать dnd строки (простая демонстрация)
   — закомментировано, чтобы не мешать обычному вводу
   static void console_on_drag_enter(Window* w, const WMDrag* d){ (void)w;(void)d; }
   static void console_on_drag_over(Window* w, WMDrag* d, int lx, int ly){ (void)lx;(void)ly; d->effect = WM_DRAG_COPY; }
   static void console_on_drag_leave(Window* w, const WMDrag* d){ (void)w;(void)d; }
   static void console_on_drop(Window* w, WMDrag* d, int lx, int ly){ (void)w;(void)lx;(void)ly; d->effect = WM_DRAG_COPY; }
*/

/* Консоль не принимает DnD: выставляем REJECT при наведении */
static void con_drag_enter(Window* w, const WMDrag* d){ (void)w;(void)d; }
static void con_drag_leave(Window* w, const WMDrag* d){ (void)w;(void)d; }
static void con_drag_over(Window* w, WMDrag* d, int lx, int ly){
    (void)w;(void)lx;(void)ly;
    d->effect = WM_DRAG_REJECT;
}
static void con_drop(Window* w, WMDrag* d, int lx, int ly){
    (void)w;(void)d;(void)lx;(void)ly; /* ничего не делаем */
}

static const WindowVTable V = {
    .draw = console_draw,
    .on_event = console_on_event,
    .tick = console_tick,
    .on_focus = NULL,
    .destroy = console_destroy,
    .on_frame_changed = console_on_frame_changed,
    .on_drag_enter = con_drag_enter,
    .on_drag_over  = con_drag_over,
    .on_drag_leave = con_drag_leave,
    .on_drop       = con_drop
};

void win_console_init(Window *w, Rect frame, int z){
    window_init(w, "console", frame, z, &V);

    ConsoleState *st = (ConsoleState*)calloc(1, sizeof(ConsoleState));
    st->col_bg = 0xFF000000;
    st->col_fg = 0xFFFFFFFF;

    console_measure(st, surface_w(w->cache), surface_h(w->cache));

    /* приветственные строки */
    const char* hello1 = "console ready. type here...";
    const char* hello2 = "press Enter to commit line";
    st->lines[0] = xstrdup(hello1); st->line_len[0] = (int)strlen(hello1);
    st->lines[1] = xstrdup(hello2); st->line_len[1] = (int)strlen(hello2);
    st->count = 2; st->head = 0;

    st->blink_on = true;
    st->next_blink_ms = SDL_GetTicks() + 500;

    w->user = st;
    w->animating = true;
    w->next_anim_ms = SDL_GetTicks();
    w->invalid_all = true;
}
