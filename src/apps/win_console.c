#include "win_console.h"
#include "../gfx/surface.h"
#include "../gfx/text.h"
#include "../core/drag.h"
#include "../core/timing.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "console/sink.h"
#include "console/store.h"
#include "console/processor.h"
#include "console/widget.h"
#include "apps/widget_color.h"
#include "apps/win_square.h" /* для SquarePayload из DnD */


typedef struct {
    /* сетка и метрики */
    int cell_w, cell_h;
    int cols, rows;

    /* курсор/мигание */
    bool  blink_on;
    uint32_t next_blink_ms;

    int   glyph_h;   /* высота строки для выравнивания по базовой линии */
    int   cursor_col; /* позиция курсора в текущей вью (рисование) */

    ConsoleStore* store;
    ConsoleSink*  sink;
    ConsoleProcessor* proc;

    /* цвета */
    uint32_t col_bg;
    uint32_t col_fg;
} ConsoleViewState;

/* ---------- utils ---------- */


static void console_measure(ConsoleViewState *st, int win_w, int win_h){
    int wM=0, hM=0;
    if (text_measure_utf8("M", &wM, &hM) != 0){ wM=8; hM=16; }
    st->cell_w = (wM>0? wM:8);
    st->cell_h = (hM>0? hM:16);
    /* для выравнивания текста к низу ячейки используем высоту строки шрифта */
    st->glyph_h = st->cell_h;

    st->cols = win_w / st->cell_w;  if (st->cols<1) st->cols=1;
    st->rows = win_h / st->cell_h;  if (st->rows<2) st->rows=2; // минимум: 1 история + 1 edit
}

/* ---- helpers: отображение истории и hit-test ---- */
typedef struct {
    int start_index;    /* первый индекс в Store, который попадает в видимую историю */
    int history_rows;   /* сколько строк истории (без edit) помещается */
} HistoryLayout;

static HistoryLayout layout_compute(ConsoleViewState* st){
    HistoryLayout L={0};
    L.history_rows = st->rows - 1; if (L.history_rows < 0) L.history_rows = 0;
    int total = con_store_count(st->store);
    int start = total - L.history_rows; if (start < 0) start = 0;
    L.start_index = start;
    return L;
}

/* Возвращает индекс элемента Store под локальной координатой (lx,ly) в истории,
   либо -1 если это зона редактирования (последняя строка) или вне видимой истории. */
static int layout_hit_item(ConsoleViewState* st, int lx, int ly, int* out_cell_x, int* out_cell_y){
    (void)lx;
    HistoryLayout L = layout_compute(st);
    int row = ly / st->cell_h;
    if (row < 0 || row >= st->rows) return -1;
    if (row == st->rows - 1) return -1; /* это edit-строка */
    if (row >= L.history_rows) return -1;
    if (out_cell_x) *out_cell_x = 0;
    if (out_cell_y) *out_cell_y = row * st->cell_h;
    return L.start_index + row;
}

/* хук окна: пересчитать метрики сетки при смене размера */
static void console_on_frame_changed(Window* w, int old_w, int old_h){
    (void)old_w; (void)old_h;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    console_measure(st, surface_w(w->cache), surface_h(w->cache));
    /* ограничим курсор по новым колонкам */
    if (st->cursor_col > st->cols) st->cursor_col = st->cols;
    /* clamp к длине edit в Store */
    char tmp[CON_MAX_LINE]; int elen = con_store_get_edit(st->store, tmp, sizeof(tmp));
    if (st->cursor_col > elen) st->cursor_col = elen;
    /* */
    w->invalid_all = true;
}

static void console_dirty_line(Window* w, ConsoleViewState* st, int vis_row){
    if (vis_row<0 || vis_row>=st->rows) return;
    Rect r = rect_make(w->frame.x, w->frame.y + vis_row*st->cell_h,
                       st->cols*st->cell_w, st->cell_h);
    window_invalidate(w, r);
}


/* ---------- draw ---------- */

static void draw_line_text(Surface *dst, int x, int y, const char *s, uint32_t argb){
    if (!s || !*s) return;
    Surface *glyph = text_render_utf8(s, argb);
    if (!glyph) return;
    surface_blit(glyph, 0,0, surface_w(glyph), surface_h(glyph), dst, x,y);
    surface_free(glyph);
}

static void console_draw(Window *w, const Rect *area){
    (void)area;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    int baseline_off = st->cell_h - st->glyph_h;

    /* очистить окно */
    surface_fill(w->cache, st->col_bg);

    HistoryLayout L = layout_compute(st);
    int vis_row = 0;

    /* рисуем историю (текст или виджеты построчно) */
    for (; vis_row < L.history_rows; ++vis_row){
        int idx = L.start_index + vis_row;
        if (idx >= con_store_count(st->store)) break;
        ConsoleWidget* cw = con_store_get_widget(st->store, idx);
        if (cw){
            /* виджет занимает всю строку */
            int y = vis_row * st->cell_h;
            if (cw->draw){
                cw->draw(cw, w->cache, 0, y, st->cols*st->cell_w, st->cell_h, st->col_fg);
            }
        } else {
            /* текстовая строка */
            const char *s = con_store_get_line(st->store, idx);
            if (!s) s = "";
            draw_line_text(w->cache, 0, vis_row * st->cell_h + baseline_off, s, st->col_fg);
        }
    }

    /* рисуем редактируемую строку (последняя) */
    int edit_y = vis_row * st->cell_h;
    {
        char eb[CON_MAX_LINE]; eb[0]=0;
        int elen = con_store_get_edit(st->store, eb, sizeof(eb));
        if (elen > 0){
            draw_line_text(w->cache, 0, edit_y + baseline_off, eb, st->col_fg);
        }
        /* курсор */
        if (st->blink_on){
            int cx = st->cursor_col * st->cell_w;
            surface_fill_rect(w->cache, cx, edit_y, 2, st->cell_h, st->col_fg);
        }
    }

    w->invalid_all = false;
}

/* --- destroy: освобождаем буферы истории --- */
static void console_destroy(Window *w){
    if (!w) return;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    if (st){
        if (st->sink) con_sink_destroy(st->sink);
        free(st);
    }
    w->user = NULL;
}

/* ---------- тик анимации (мигание курсора) ---------- */

static void console_tick(Window *w, uint32_t now){
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    if (now >= st->next_blink_ms){
        st->blink_on = !st->blink_on;
        st->next_blink_ms = now + 500;
        /* грязним только строку курсора */
        console_dirty_line(w, st, st->rows - 1);
    }
    w->next_anim_ms = next_frame(now);
}

/* уведомление от Store: помечаем окно к перерисовке */
static void on_store_changed(void* user){
    Window* w = (Window*)user;
    if (!w) return;
    w->invalid_all = true;
}

/* ---------- ввод ---------- */

static void console_on_event(Window *w, void* wm, const InputEvent *e, int lx, int ly){
    (void)lx; (void)ly;
    ConsoleViewState *st = (ConsoleViewState*)w->user;

    (void)wm;

    /* Сначала — мышь к виджетам в истории (если попали) */
    if (e->type==3 || e->type==4 || e->type==5){
        int cell_x=0, cell_y=0;
        int idx = layout_hit_item(st, lx, ly, &cell_x, &cell_y);
        if (idx >= 0){
            ConsoleWidget* cw = con_store_get_widget(st->store, idx);
            if (cw && cw->on_event){
                int wx = lx - cell_x;
                int wy = ly - cell_y;
                int changed = cw->on_event(cw, e, wx, wy, st->cols*st->cell_w, st->cell_h);
                if (changed){
                    /* Виджет изменился — перерисовать строку и уведомить всех слушателей Store */
                    con_store_notify_changed(st->store);
                    Rect r = rect_make(w->frame.x, w->frame.y + cell_y, st->cols*st->cell_w, st->cell_h);
                    window_invalidate(w, r);
                    w->invalid_all = true;
                }
            }
            return; /* событие «съедено» виджетом */
        }
    }

    if (e->type == 2){ /* TEXTINPUT */
        /* Повторяем старую семантику wrap по колонкам во View */
        const char *p = e->text.text;
        while (*p){
            char buf[2] = { *p++, 0 };
            con_sink_submit_text(st->sink, e->user_id, e->text.text);
            st->cursor_col++;
            if (st->cursor_col >= st->cols){
                /* мягкий перенос строки: коммитим только в Store, без вызова процессора */
                con_store_commit(st->store);
                st->cursor_col = 0;
            }
        }
        console_dirty_line(w, st, st->rows-1);
        w->invalid_all = true;
    } else if (e->type == 1){ /* KEYDOWN */
        int sym = e->key.sym;
        if (sym == SDLK_BACKSPACE){
            con_sink_backspace(st->sink, e->user_id);
            if (st->cursor_col>0) st->cursor_col--;
            console_dirty_line(w, st, st->rows-1);
            w->invalid_all = true;
        } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER){
            con_sink_commit(st->sink, e->user_id); /* Enter: отправка в процессор */
            st->cursor_col = 0;
            /* перерисовать весь видимый блок (история сдвинулась) */
            w->invalid_all = true;
        } else if (sym == SDLK_HOME){
            st->cursor_col = 0; console_dirty_line(w, st, st->rows-1); w->invalid_all = true;
        } else if (sym == SDLK_END){
            char tmp[CON_MAX_LINE]; int elen = con_store_get_edit(st->store, tmp, sizeof(tmp));
            st->cursor_col = elen;
            if (st->cursor_col > st->cols) st->cursor_col = st->cols;
            console_dirty_line(w, st, st->rows-1); w->invalid_all = true;
        }
    }
}

/* ---------- vtable и init ---------- */

/* ---- Drag&Drop: разрешаем DROP некоторых типов для вставки интерактива ---- */

static void con_drag_enter(Window* w, const WMDrag* d){ (void)w;(void)d; }
static void con_drag_leave(Window* w, const WMDrag* d){ (void)w; (void)d; }
static void con_drag_over(Window* w, WMDrag* d, int lx, int ly){
    (void)w; (void)lx; (void)ly;
    if (!d || !d->mime) { d->effect = WM_DRAG_NONE; return; }
    if (strcmp(d->mime, "application/x-square")==0){
        d->effect = WM_DRAG_COPY; /* покажем, что можем вставить в консоль */
    } else {
        d->effect = WM_DRAG_REJECT;
    }
}

static void con_drop(Window* w, WMDrag* d, int lx, int ly){
    (void)lx; (void)ly;
    if (!d || !d->mime) return;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    if (strcmp(d->mime, "application/x-square")==0){
        /* Создаём интерактивный ColorSlider (демо): управляет глобальной переменной R */
        uint8_t initial = 128;
        if (d->size >= (int)sizeof(SquarePayload) && d->data){
            const SquarePayload* sp = (const SquarePayload*)d->data;
            initial = (uint8_t)((sp->colA >> 16) & 0xFF);
        }
        ConsoleWidget* cw = widget_color_create(initial);
        if (cw){
            con_store_append_widget(st->store, cw);
            con_store_notify_changed(st->store);
            w->invalid_all = true;
            d->effect = WM_DRAG_COPY;
        } else {
            d->effect = WM_DRAG_REJECT;
        }
    }
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


void win_console_init(Window *w, Rect frame, int z, ConsoleStore* store, ConsoleProcessor* proc){
    window_init(w, "console", frame, z, &V);

    ConsoleViewState *st = (ConsoleViewState*)calloc(1, sizeof(ConsoleViewState));
    st->col_bg = 0xFF000000;
    st->col_fg = 0xFFFFFFFF;

    console_measure(st, surface_w(w->cache), surface_h(w->cache));

    /* модель и sink */
    st->store = store;
    st->proc  = proc;
    st->sink  = con_sink_create(store, proc);
    /* подписка на изменения Store — чтобы вторая вьюха увидела обновления */
    con_store_subscribe(store, on_store_changed, w);

    st->blink_on = true;
    st->next_blink_ms = SDL_GetTicks() + 500;

    w->user = st;
    w->animating = true;
    w->next_anim_ms = SDL_GetTicks();
    w->invalid_all = true;
}
