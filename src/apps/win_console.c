#include "win_console.h"
#include "../gfx/surface.h"
#include "../gfx/text.h"
#include "../core/drag.h"
#include "../core/timing.h"
#include "../core/wm.h"
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
#include "console/delta.h"

#define MIME_CMD_TEXT "application/x-console-cmd-text"

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

    /* построчная «грязь» для частичного перерисовывания (до 64 строк) */
    uint64_t dirty_rows_mask; /* бит i => строка i требует перерисовки */
    /* прим.: если строк >64, fallback: dirty_rows_mask==0 => полный redraw */

    /* DnD-источник для текстовых строк истории */
    int   drag_arm;         /* 1 — «вооружён» стартовать DnD после порога */
    int   drag_start_mx;    /* экранные координаты press для порога */
    int   drag_start_my;
    int   drag_src_index;   /* индекс строки истории под курсором при press */
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
    st->dirty_rows_mask = 0; /* смена размера — проще перерисовать всё */
}

static void console_dirty_line(Window* w, ConsoleViewState* st, int vis_row){
    if (vis_row<0 || vis_row>=st->rows) return;
    Rect r = rect_make(w->frame.x, w->frame.y + vis_row*st->cell_h,
                       st->cols*st->cell_w, st->cell_h);
    window_invalidate(w, r);
    /* отметим строку как «грязную» для частичного redraw */
    if (vis_row < 64){
        st->dirty_rows_mask |= (1ull << (uint64_t)vis_row);
    } else {
        st->dirty_rows_mask = 0; /* слишком много строк — редравим всё */
    }
}


/* ---------- draw ---------- */

static void draw_line_text(Surface *dst, int x, int y, const char *s, uint32_t argb){
    if (!s || !*s) return;
    Surface *glyph = text_render_utf8(s, argb);
    if (!glyph) return;
    surface_blit(glyph, 0,0, surface_w(glyph), surface_h(glyph), dst, x,y);
    surface_free(glyph);
}

/* helper для частичного редрава: отрисовать одну строку истории */
static void s_draw_history_row(Window* w, ConsoleViewState* st, const HistoryLayout* L,
                               int row, int baseline_off){
    if (!w || !st || !L) return;
    if (row < 0 || row >= L->history_rows) return;
    int idx = L->start_index + row;
    if (idx < 0 || idx >= con_store_count(st->store)) return;
    int y = row * st->cell_h;
    /* затираем фон полосы строки */
    surface_fill_rect(w->cache, 0, y, st->cols*st->cell_w, st->cell_h, st->col_bg);
    ConsoleWidget* cw = con_store_get_widget(st->store, idx);
    if (cw && cw->draw){
        cw->draw(cw, w->cache, 0, y, st->cols*st->cell_w, st->cell_h, st->col_fg);
    } else {
        const char *s = con_store_get_line(st->store, idx);
        if (!s) s = "";
        draw_line_text(w->cache, 0, y + baseline_off, s, st->col_fg);
    }
}


static void console_draw(Window *w, const Rect *area){
    (void)area;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    int baseline_off = st->cell_h - st->glyph_h;

    /* очистить окно */
    if (st->dirty_rows_mask == 0){
        /* полный кадр */
        surface_fill(w->cache, st->col_bg);
    }

    HistoryLayout L = layout_compute(st);
    int vis_row = 0;


    if (st->dirty_rows_mask == 0){
        /* обычный путь: рисуем все видимые строки истории */
        for (; vis_row < L.history_rows; ++vis_row){
            s_draw_history_row(w, st, &L, vis_row, baseline_off);
        }
    } else {
        /* частичная перерисовка: только грязные строки истории */
        uint64_t m = st->dirty_rows_mask;
        for (int row = 0; row < L.history_rows && m; ++row){
            if (m & 1ull){ s_draw_history_row(w, st, &L, row, baseline_off); }
            m >>= 1ull;
        }
    }

    /* рисуем редактируемую строку (последняя) */
    int edit_y = vis_row * st->cell_h;
    if (st->dirty_rows_mask == 0){
        /* перерисовываем как обычно весь edit-ряд */
        surface_fill_rect(w->cache, 0, edit_y, st->cols*st->cell_w, st->cell_h, st->col_bg);
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
    } else {
        /* перерисовываем edit-ряд только если он помечен грязным */
        int need = (st->rows-1 < 64) ? ((st->dirty_rows_mask >> (st->rows-1)) & 1ull) : 1;
        if (need){
            surface_fill_rect(w->cache, 0, edit_y, st->cols*st->cell_w, st->cell_h, st->col_bg);
            char eb[CON_MAX_LINE]; eb[0]=0;
            int elen = con_store_get_edit(st->store, eb, sizeof(eb));
            if (elen > 0){
                draw_line_text(w->cache, 0, edit_y + baseline_off, eb, st->col_fg);
            }
            if (st->blink_on){
                int cx = st->cursor_col * st->cell_w;
                surface_fill_rect(w->cache, cx, edit_y, 2, st->cell_h, st->col_fg);
            }
        }
    }
    w->invalid_all = false;
    st->dirty_rows_mask = 0; /* сбрасываем частичную «грязь» */
}

/* M10: маленькое превью для перетаскиваемой команды */
static Surface* make_cmd_preview(const char* s){
    if (!s) return NULL;
    int tw=0, th=0;
    if (text_measure_utf8(s, &tw, &th) != 0){ tw=120; th=16; }
    /* ограничим ширину превью, чтобы не занимало весь экран */
    const int PAD = 8;
    const int MAX_W = 260;
    int gw = tw, gh = th;
    int pw = gw + PAD*2; if (pw > MAX_W) pw = MAX_W;
    int ph = gh + PAD*2;
    Surface* prev = surface_create_argb(pw, ph);
    if (!prev) return NULL;
    /* фон + рамка */
    surface_fill(prev, 0xAA000000);
    surface_fill_rect(prev, 1,1, pw-2, ph-2, 0xFF202020);
    /* сам текст (усечём по ширине) */
    Surface* glyph = text_render_utf8(s, 0xFFFFFFFF);
    if (glyph){
        int blit_w = surface_w(glyph); if (blit_w > pw - PAD*2) blit_w = pw - PAD*2;
        surface_blit(glyph, 0,0, blit_w, surface_h(glyph), prev, PAD, PAD);
        surface_free(glyph);
    }
    return prev;
}

/* --- destroy: освобождаем буферы истории --- */
static void console_destroy(Window *w){
    if (!w) return;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    if (st){
        /* sink принадлежит внешнему коду (main), не уничтожаем здесь */
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
        /* mask уже проставлен в console_dirty_line */
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
                    /* Эмиссия дельты: сериализуем текущее состояние виджета и отправляем через Sink.
                       дельта имеет нормализованный заголовок (LWW, commutative). */
                    /* Эмиссия дельты: отправляем ТОЛЬКО нормализованный пакет "cw.delta" (LWW, commutative). */
                    ConItemId wid = con_store_get_id(st->store, idx);
                    if (wid != CON_ITEMID_INVALID && st->sink){
                        if (cw->get_state_blob){
                            struct {
                                ConDeltaHdr h;
                                int v;
                            } pkt;
                            size_t slen = sizeof(int);
                            if (cw->get_state_blob(cw, &pkt.v, &slen) && slen==sizeof(int)){
                                pkt.h.schema   = CON_DELTA_SCHEMA_V1;
                                pkt.h.kind     = CON_DELTA_KIND_LWW_SET;
                                pkt.h.flags    = 0;
                                pkt.h.hlc      = con_sink_tick_hlc(st->sink, SDL_GetTicks());
                                pkt.h.actor_id = con_sink_get_actor_id(st->sink);
                                pkt.h.reserved = 0;
                                con_sink_widget_delta(st->sink, e->user_id, wid, "cw.delta", &pkt, sizeof(pkt));
                            }
                        }
                    } else {
                        /* как раньше — локальное уведомление (на случай отсутствия sink) */
                        con_store_notify_changed(st->store);
                    }
                    console_dirty_line(w, st, ly / st->cell_h); /* строка с виджетом */
                    Rect r = rect_make(w->frame.x, w->frame.y + cell_y, st->cols*st->cell_w, st->cell_h);
                    window_invalidate(w, r);
                    w->invalid_all = true;
                }
            }
            return; /* событие «съедено» виджетом */
        }
    }

    /* DnD-источник для текстовых строк истории */
    if (e->type==3){ /* mouse button */
        if (e->mouse.button==1 && e->mouse.state==1){
            /* press — проверим, попали ли по текстовой строке истории */
            int cell_x=0, cell_y=0;
            int idx = layout_hit_item(st, lx, ly, &cell_x, &cell_y);
            if (idx >= 0){
                /* не стартуем drag для строк-виджетов */
                ConsoleWidget* cw = con_store_get_widget(st->store, idx);
                if (!cw){
                    st->drag_arm = 1;
                    st->drag_start_mx = e->mouse.x;
                    st->drag_start_my = e->mouse.y;
                    st->drag_src_index = idx;
                }
            }
        } else if (e->mouse.button==1 && e->mouse.state==0){
            /* отпускание — если не стартовали dnd, просто сбросить arm */
            st->drag_arm = 0;
            st->drag_src_index = -1;
        }
    } else if (e->type==4 && (e->mouse.buttons & 1)){
        if (st->drag_arm){
            int dx = e->mouse.x - st->drag_start_mx;
            int dy = e->mouse.y - st->drag_start_my;
            if (dx*dx + dy*dy >= 9){ /* порог ~3px */
                /* извлечём текст команды и запустим DnD */
                int idx = st->drag_src_index;
                const char* line = NULL;
                size_t line_len = 0;
                if (idx >= 0 && idx < con_store_count(st->store)){
                    line = con_store_get_line(st->store, idx);
                    line_len = (size_t)con_store_get_line_len(st->store, idx);
                }
                if (line && line_len > 0){
                    Surface* prev = make_cmd_preview(line);
                    /* горячая точка — небольшой отступ, чтобы палец/курсор не закрывал текст */
                    int hot = 6;
                    wm_start_drag((WM*)wm, e->user_id, w,
                                  MIME_CMD_TEXT,
                                  (void*)line, line_len,
                                  prev, hot, hot);
                    /* примечание: prev освободится на wm_end_drag() */
                }
                st->drag_arm = 0;
                st->drag_src_index = -1;
                /* дальше событие будет обрабатываться протоколом DnD в input_route_mouse */
                return;
            }
        }
    }

    if (e->type == 2){ /* TEXTINPUT */
        /* Повторяем старую семантику wrap по колонкам во View */
        const char *p = e->text.text;
        while (*p){
            char buf[2] = { *p++, 0 };
            con_sink_submit_text(st->sink, e->user_id, buf);
            st->cursor_col++;
            if (st->cursor_col >= st->cols){
                /* мягкий перенос строки: коммитим только в Store, без вызова процессора */
                con_store_commit(st->store);
                /* после смещения истории перерисуем всю видимую область */
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
    } else if (strcmp(d->mime, MIME_CMD_TEXT)==0){
        d->effect = WM_DRAG_COPY;
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
            ConItemId id = con_store_append_widget(st->store, cw);
            con_store_notify_changed(st->store);
            w->invalid_all = true;
            (void)id; /* в М4 мы ещё не отображаем ID во view, но он есть */
            d->effect = WM_DRAG_COPY;
        } else {
            d->effect = WM_DRAG_REJECT;
        }
    } else if (strcmp(d->mime, MIME_CMD_TEXT)==0){
        /* DnD текстовой команды: payload — UTF-8 (может быть не \0-terminated) */
        if (st->sink && d->data && d->size>0){
            char tmp[CON_MAX_LINE];
            size_t n = d->size; if (n >= sizeof(tmp)) n = sizeof(tmp)-1;
            memcpy(tmp, d->data, n);
            tmp[n] = 0;
            /* Добавим как строку в историю и сразу выполним процессором (не трогая edit) */
            con_sink_commit_text_command(st->sink, 0/*user_id*/, tmp);
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


void win_console_init(Window *w, Rect frame, int z, ConsoleStore* store, ConsoleProcessor* proc, ConsoleSink* sink){
    window_init(w, "console", frame, z, &V);

    ConsoleViewState *st = (ConsoleViewState*)calloc(1, sizeof(ConsoleViewState));
    st->col_bg = 0xFF000000;
    st->col_fg = 0xFFFFFFFF;

    console_measure(st, surface_w(w->cache), surface_h(w->cache));

    /* модель и sink */
    st->store = store;
    st->proc  = proc;
    st->sink  = sink;
    /* подписка на изменения Store — чтобы вторая вьюха увидела обновления */
    con_store_subscribe(store, on_store_changed, w);

    st->blink_on = true;
    st->next_blink_ms = SDL_GetTicks() + 500;
    st->dirty_rows_mask = 0;

    w->user = st;
    w->animating = true;
    w->next_anim_ms = SDL_GetTicks();
    w->invalid_all = true;
}
