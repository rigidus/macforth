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
#include "console/prompt.h"

#define MIME_CMD_TEXT "application/x-console-cmd-text"

/* Цвета пользователей для бордеров и окраски команд */
static const uint32_t USER_COLORS[2] = { 0xFF3B82F6, /* user0: синий */ 0xFF22C55E /* user1: зелёный */ };

/* Максимум строк, для которых имеет смысл держать битовую маску грязи.
   Если строк больше — при точечных изменениях лучше перерисовать всё. */
#define CON_MAX_VIEW_ROWS 64

typedef struct {
    /* сетка и метрики */
    int cell_w, cell_h;
    int cols, rows;

    int   glyph_h;   /* высота строки для выравнивания по базовой линии */

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
    int        drag_is_placeholder;            /* 1 если источником служит плейсхолдер */
    ConItemId  pending_placeholder_id;         /* для click-активации, если drag не стартовал */
    char       drag_text_buf[CON_MAX_LINE];    /* текст для DnD из плейсхолдера */

    ConsolePrompt* prompt;  /* единственный промпт внизу */
    int            prompt_user_id;
    int            top_h;   /* всегда 0 */
    int            bot_h;   /* высота области промпта */

    /* ---- Кэш лейаута row→id ---- */
    int       layout_rows;        /* сколько строк истории помещается (без промпта) */
    int       layout_start_index; /* индекс первого элемента Store в окне */
    int       layout_valid;       /* 1 — кэш соответствует текущему Store/размеру */
    ConItemId row_ids[CON_MAX_VIEW_ROWS]; /* id элемента для каждой видимой строки (0, если нет) */

    /* Для пометки all-redraw (когда структура изменилась) */
    int       request_full_redraw;

    /* ---- Lazy mode (per-view) ---- */
    LazyMode   lazy_mode;
    ConItemId  active_for_user[WM_MAX_USERS];  /* какой widget активен у каждого user на этой вьюхе */
    int        chord_stage[WM_MAX_USERS];      /* 0 – нет, 1 – C-x, 2 – C-x w (ждём g) */

    WM*       wm;              /* back-pointer для броска damage при инвалидации */
} ConsoleViewState;

/* ---------- utils ---------- */

/* fwd: используем ниже до определения */
static void draw_border_rect(Surface* dst, int x,int y,int w,int h, uint32_t col);

static void console_measure(ConsoleViewState *st, int win_w, int win_h){
    int wM=0, hM=0;
    if (text_measure_utf8("M", &wM, &hM) != 0){ wM=8; hM=16; }
    st->cell_w = (wM>0? wM:8);
    st->cell_h = (hM>0? hM:16);
    /* для выравнивания текста к низу ячейки используем высоту строки шрифта */
    st->glyph_h = st->cell_h;

    st->cols = win_w / st->cell_w;  if (st->cols<1) st->cols=1;
    st->rows = win_h / st->cell_h;  if (st->rows<1) st->rows=1; // минимум: 1 строка истории

}

/* ---- helpers: отображение истории и hit-test ---- */
typedef struct {
    int start_index;    /* первый индекс в Store, который попадает в видимую историю */
    int history_rows;   /* сколько строк истории помещается (вся высота окна) */
} HistoryLayout;

static HistoryLayout layout_compute(ConsoleViewState* st){
    HistoryLayout L={0};
    /* rows задаются при изменении кадра (console_on_frame_changed) */
    L.history_rows = st->rows;
    int total = con_store_count(st->store);
    int start = total - L.history_rows; if (start < 0) start = 0;
    L.start_index = start;
    return L;
}

/* Пересобрать кэш лейаута row→id (только если влезает в CON_MAX_VIEW_ROWS) */
static void layout_rebuild_cache(ConsoleViewState* st){
    HistoryLayout L = layout_compute(st);
    st->layout_rows = L.history_rows;
    st->layout_start_index = L.start_index;
    st->layout_valid = 1;
    if (L.history_rows > CON_MAX_VIEW_ROWS){
        /* слишком много строк — кэш не используем, fallback на полный redraw */
        st->layout_valid = 0;
        return;
    }
    for (int row=0; row<L.history_rows; ++row){
        int idx = L.start_index + row;
        ConItemId id = con_store_get_id(st->store, idx);
        st->row_ids[row] = id;
    }
}

/* Найти видимую строку по id, используя кэш; вернуть -1, если нет */
static int cached_find_row_by_id(ConsoleViewState* st, ConItemId id){
    if (!st->layout_valid) return -1;
    for (int r=0; r<st->layout_rows; ++r){
        if (st->row_ids[r] == id) return r;
    }
    return -1;
}

static void mark_row_dirty(Window* w, ConsoleViewState* st, int row){
    if (row < 0) return;
    if (row >= st->rows) return;
    if (st->rows > CON_MAX_VIEW_ROWS) {
        /* Полный redraw окна: кидаем damage на весь frame */
        if (st->wm) wm_window_invalidate(st->wm, w, w->frame); else w->invalid_all = true;
        return;
    }
    st->dirty_rows_mask |= (1ull << row);
    int y = st->top_h + row * st->cell_h;
    Rect r = rect_make(w->frame.x, w->frame.y + y, st->cols*st->cell_w, st->cell_h);
    if (st->wm) wm_window_invalidate(st->wm, w, r); else window_invalidate(w, r);
}


/* Возвращает индекс элемента Store под локальной координатой (lx,ly) в истории,
   либо -1 если вне видимой истории или попали в промпты. */
static int layout_hit_item(ConsoleViewState* st, int lx, int ly, int* out_cell_x, int* out_cell_y){
    (void)lx;
    int row = (ly - st->top_h) / st->cell_h;
    HistoryLayout L = layout_compute(st);
    if (row < 0 || row >= L.history_rows) return -1;
    if (out_cell_x) *out_cell_x = 0;
    if (out_cell_y) *out_cell_y = st->top_h + row * st->cell_h;
    return L.start_index + row;
}

/* хук окна: пересчитать метрики сетки при смене размера */
static void console_on_frame_changed(Window* w, int old_w, int old_h){
    (void)old_w; (void)old_h;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    console_measure(st, surface_w(w->cache), surface_h(w->cache));
    /* нижний промпт: по метрике шрифта + отступы */
    int wM=0,hM=0; text_measure_utf8("M",&wM,&hM);
    int pad = 6;
    st->top_h = 0;
    st->bot_h = (st->prompt ? (hM + pad*2) : 0);
    /* сколько строк в истории помещается до промпта */
    int middle_h = surface_h(w->cache) - st->bot_h;
    if (middle_h < st->cell_h) middle_h = st->cell_h;
    st->rows = middle_h / st->cell_h;
    /* сбрасываем кэш и маску */
    st->layout_valid = 0;
    st->dirty_rows_mask = 0;
    st->request_full_redraw = 1;
    w->invalid_all = true;
    st->dirty_rows_mask = 0; /* смена размера — проще перерисовать всё */
}


/* ---------- draw ---------- */

static void draw_line_text(Surface *dst, int x, int y, const char *s, uint32_t argb){
    if (!s || !*s) return;
    Surface *glyph = text_render_utf8(s, argb);
    if (!glyph) return;
    surface_blit(glyph, 0,0, surface_w(glyph), surface_h(glyph), dst, x,y);
    surface_free(glyph);
}

/* Текст для снапшота */
static void draw_snapshot_line(Surface* dst, int x, int y, int baseline_off, int dropped, uint32_t fg){
    char line[96];
    SDL_snprintf(line, sizeof(line), "(... %d older lines compacted ...)", dropped);
    draw_line_text(dst, x, y + baseline_off, line, fg);
}

static void ensure_layout(ConsoleViewState* st){
    if (!st->layout_valid || st->request_full_redraw){
        layout_rebuild_cache(st);
        st->request_full_redraw = 0;
    }
}

/* helper для частичного редрава: отрисовать одну строку истории */
static void s_draw_history_row(Window* w, ConsoleViewState* st, const HistoryLayout* L,
                               int row, int baseline_off){
    if (!w || !st || !L) return;
    if (row < 0 || row >= L->history_rows) return;
    int idx = L->start_index + row;
    if (idx < 0 || idx >= con_store_count(st->store)) return;
    int y = st->top_h + row * st->cell_h;
    /* затираем фон полосы строки */
    surface_fill_rect(w->cache, 0, y, st->cols*st->cell_w, st->cell_h, st->col_bg);
    ConEntryType et = con_store_get_type(st->store, idx);
    ConsoleWidget* cw = (et==CON_ENTRY_WIDGET) ? con_store_get_widget(st->store, idx) : NULL;
    if (cw && cw->draw){
        /* ленивый плейсхолдер? */
        ConItemId id = con_store_get_id(st->store, idx);
        int is_active = (st->active_for_user[st->prompt_user_id] == id);
        int show_placeholder = (st->lazy_mode != LAZY_OFF) && !is_active;
        if (st->lazy_mode == LAZY_ALWAYS_TEXT) show_placeholder = 1;
        if (show_placeholder){
            /* затемнённая плашка + текстовое представление виджета */
            uint32_t plate = 0xFF141414; /* тёмно-серая плашка на чёрном фоне */
            surface_fill_rect(w->cache, 0, y, st->cols*st->cell_w, st->cell_h, plate);
            char buf[128]; buf[0]=0;
            const char* txt = cw->as_text ? cw->as_text(cw, buf, (int)sizeof(buf)) : "[widget]";
            draw_line_text(w->cache, 6, y + baseline_off, txt ? txt : "[widget]", 0xFFCCCCCC);
            /* рамка: неактивный — приглушённая */
            draw_border_rect(w->cache, 0, y, st->cols*st->cell_w, st->cell_h, 0xFF353535);
        } else {
            /* активен — рисуем интерактив и яркую рамку пользователя */
            cw->draw(cw, w->cache, 0, y, st->cols*st->cell_w, st->cell_h, st->col_fg);
            uint32_t bcol = USER_COLORS[st->prompt_user_id & 1];
            draw_border_rect(w->cache, 0, y, st->cols*st->cell_w, st->cell_h, bcol);
        }
    } else {
        if (et == CON_ENTRY_SNAPSHOT){
            int dropped = con_store_get_snapshot_dropped(st->store, idx);
            /* всегда системный цвет для снапшотов */
            draw_snapshot_line(w->cache, 0, y, baseline_off, (dropped>=0? dropped:0), 0xFFAAAAAA);
            /* рамку не рисуем */
            return;
        }
        const char *s = (et==CON_ENTRY_TEXT) ? con_store_get_line(st->store, idx) : "";
        if (!s) s = "";
        /* выбираем цвет текста по user_id источника */
        int uid = con_store_get_user(st->store, idx);
        uint32_t col = st->col_fg;
        if (uid==0) col = USER_COLORS[0];
        else if (uid==1) col = USER_COLORS[1];
        draw_line_text(w->cache, 0, y + baseline_off, s, col);
        /* для обычных текстовых строк рамку не рисуем */
    }
}




static void draw_border_rect(Surface* dst, int x,int y,int w,int h, uint32_t col){
    surface_fill_rect(dst, x, y, w, 1, col);
    surface_fill_rect(dst, x, y+h-1, w, 1, col);
    surface_fill_rect(dst, x, y, 1, h, col);
    surface_fill_rect(dst, x+w-1, y, 1, h, col);
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

    ensure_layout(st);
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
        /* маску отрисовали — сбросим */
        st->dirty_rows_mask = 0;
    }

    /* --- нижний промпт (для prompt_user_id) --- */
    if (st->prompt){
        int y0 = surface_h(w->cache) - st->bot_h;
        con_prompt_set_colors(st->prompt, 0xFF0A0A0A, 0xFFFFFFFF);
        con_prompt_draw(st->prompt, w->cache, 4, y0+4, surface_w(w->cache)-8, st->bot_h-8);
        draw_border_rect(w->cache, 2, y0+2, surface_w(w->cache)-4, st->bot_h-4, USER_COLORS[st->prompt_user_id & 1]);
        /* --- индикаторы «кто-то печатает» для других пользователей --- */
        int glyph_h=16, dummy_w=8; text_measure_utf8("M",&dummy_w,&glyph_h);
        int label_y = y0 + st->bot_h - glyph_h - 2;
        for (int uid=0; uid<2 /* демо: 0..1 */; ++uid){
            if (uid == st->prompt_user_id) continue;
            int nonempty=0, edits=0;
            con_store_prompt_get_meta(st->store, uid, &nonempty, &edits);
            if (nonempty){
                char msg[64];
                SDL_snprintf(msg, sizeof(msg), "user_%d typing [%d]", uid, edits);
                Surface* g = text_render_utf8(msg, USER_COLORS[uid & 1]);
                if (g){
                    /* рисуем справа от поля */
                    int gx = surface_w(w->cache) - surface_w(g) - 8;
                    if (gx < 8) gx = 8;
                    surface_blit(g, 0,0, surface_w(g), surface_h(g), w->cache, gx, label_y);
                    surface_free(g);
                }
            }
        }
    }
    w->invalid_all = false;
    st->dirty_rows_mask = 0; /* сбрасываем частичную «грязь» */
}

/* маленькое превью для перетаскиваемой команды */
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
    /* тики промптов (мигание курсора внутри них) */
    if (st->prompt) con_prompt_tick(st->prompt, now);
    /* просто перерисуем всё окно раз в кадр (промпты компактные) */
    w->invalid_all = true;
    w->next_anim_ms = next_frame(now);
}

/* уведомление от Store: помечаем окно к перерисовке */
static void on_store_changed(void* user){
    Window* w = (Window*)user;
    if (!w) return;
    /* Попробуем «слить» точечные изменения из Store и отметить грязные строки,
       иначе — полный redraw. */
    ConsoleViewState* st = (ConsoleViewState*)w->user;
    if (!st) { w->invalid_all = true; return; }

    ConItemId ids[CON_STORE_CHANGES_MAX];
    int all = 0;
    int n = con_store_drain_changes(st->store, ids, (int)(sizeof(ids)/sizeof(ids[0])), &all);
    if (all || st->rows > CON_MAX_VIEW_ROWS){
        st->layout_valid = 0;
        st->dirty_rows_mask = 0;
        st->request_full_redraw = 1;
        /* Полный redraw: сразу добавим damage, чтобы композитор сделал кадр без ввода */
        if (st->wm) wm_window_invalidate((WM*)st->wm, w, w->frame);
        else w->invalid_all = true;
        return;
    }
    /* Если Store прислал notify без точечных изменений (например, поменялись
       только метаданные промптов: nonempty/edits) — перерисуем полосу промпта. */
    if (n == 0){
        int y0 = surface_h(w->cache) - st->bot_h;
        Rect r = rect_make(w->frame.x, w->frame.y + y0, surface_w(w->cache), st->bot_h);
        if (st->wm) wm_window_invalidate(st->wm, w, r);
        else        w->invalid_all = true;
        return;
    }
    /* есть точечные изменения — отметим соответствующие строки */
    ensure_layout(st);
    for (int i=0;i<n;i++){
        int row = cached_find_row_by_id(st, ids[i]);
        if (row >= 0) mark_row_dirty(w, st, row);
        else { /* элемент вне видимой области — можно игнорировать; если сомневаемся — redraw */
            /* no-op */
        }
    }
}

/* --- утилита: инвалидация строки по ID --- */
static void invalidate_item_by_id(Window* w, ConsoleViewState* st, ConItemId id){
    if (!id) return;
    int idx = con_store_find_index_by_id(st->store, id);
    if (idx < 0) return;
    HistoryLayout L = layout_compute(st);
    if (idx < L.start_index || idx >= L.start_index + L.history_rows) return;
    int row = idx - L.start_index;
    /* локально помечаем грязной ровно одну строку */
    st->layout_valid = 0; /* порядок мог не измениться, но перестроить кэш дёшево */
    mark_row_dirty(w, st, row);
}

/* --- обработка chord'а C-x w g (per-user) --- */
static int handle_chord_maybe(Window* w, ConsoleViewState* st, int uid, const InputEvent* e){
    if (e->type != 1) return 0;
    int stage = st->chord_stage[uid];
    if ((e->key.mods & KEYMOD_CTRL) && e->key.sym == SDLK_x){
        st->chord_stage[uid] = 1; return 1;
    }
    if (stage == 1 && e->key.sym == SDLK_w){ st->chord_stage[uid] = 2; return 1; }
    if (stage == 2 && e->key.sym == SDLK_g){
        st->chord_stage[uid] = 0;
        ConItemId prev = st->active_for_user[uid];
        if (prev){ st->active_for_user[uid] = 0; invalidate_item_by_id(w, st, prev); }
        return 1;
    }
    /* любой другой keydown сбрасывает стадию */
    st->chord_stage[uid] = 0; return 0;
}

/* ---------- ввод ---------- */

static void console_on_event(Window *w, void* wm, const InputEvent *e, int lx, int ly){
    (void)lx; (void)ly;
    ConsoleViewState *st = (ConsoleViewState*)w->user;

    (void)wm;

    int H = surface_h(w->cache);
    int in_bot_prompt = (ly >= H - st->bot_h && ly < H);

    /* Chord «C-x w g» — сворачивание активного виджета для данного user_id */
    if (e->type==1 && e->user_id == st->prompt_user_id){
        if (handle_chord_maybe(w, st, e->user_id, e)){
            return; /* chord съеден */
        }
    }

    /* Клавиатура/текст - привязка к промпту по user_id (0 → верх, 1 → низ) */
    if (e->type==1 || e->type==2){
        ConsolePrompt* tgt = (e->user_id == st->prompt_user_id) ? st->prompt : NULL;
        if (tgt){
            if (con_prompt_on_event(tgt, e)) { w->invalid_all = true; }
            return;
        }
    }

    /* Сначала — мышь к виджетам в истории (если попали) */
    if (e->type==3 || e->type==4 || e->type==5){
        int cell_x=0, cell_y=0;
        int idx = (!in_bot_prompt) ? layout_hit_item(st, lx, ly, &cell_x, &cell_y) : -1;
        if (idx >= 0){
            ConsoleWidget* cw = con_store_get_widget(st->store, idx);
            ConItemId wid2 = con_store_get_id(st->store, idx);
            if (cw && cw->on_event){
                /* ленивый режим: если виджет не активен — клик активирует,
                   но даём возможность начать DnD текста из плейсхолдера. */
                int is_active = (st->active_for_user[e->user_id] == wid2);
                int is_placeholder = 0;
                if (st->lazy_mode != LAZY_OFF && !is_active) {
                    if (st->lazy_mode == LAZY_ALWAYS_TEXT) {
                        is_placeholder = 1; /* активировать нельзя */
                    } else {
                        is_placeholder = 1; /* может активироваться по click */
                    }
                }
                /* --- placeholder как текстовая строка: даём DnD-источник и активацию по click --- */
                if (is_placeholder){
                    if (e->type==3 && e->mouse.button==1 && e->mouse.state==1){
                        /* arm DnD + помним, что это placeholder, чтобы по отпусканию активировать */
                        st->drag_arm = 1;
                        st->drag_start_mx = e->mouse.x; st->drag_start_my = e->mouse.y;
                        st->drag_src_index = idx;
                        st->drag_is_placeholder = 1;
                        st->pending_placeholder_id = (st->lazy_mode==LAZY_TEXT_UNTIL_CLICK)? wid2 : 0;
                        /* подготовим текст для возможного DnD */
                        char tmp[128]; tmp[0]=0;
                        const char* t = cw->as_text ? cw->as_text(cw, tmp, (int)sizeof(tmp)) : "[widget]";
                        SDL_strlcpy(st->drag_text_buf, t ? t : "[widget]", sizeof(st->drag_text_buf));
                        return;
                    }
                    /* мышь/колёса поверх плейсхолдера внутрь виджета не пускаем */
                    return;
                }
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
                    /* перерисуем строку с виджетом */
                    Rect r = rect_make(w->frame.x, w->frame.y + cell_y, st->cols*st->cell_w, st->cell_h);
                    if (st->wm) wm_window_invalidate(st->wm, w, r); else { window_invalidate(w, r); w->invalid_all = true; }
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
            int idx = (!in_bot_prompt) ? layout_hit_item(st, lx, ly, &cell_x, &cell_y) : -1;
            if (idx >= 0){
                ConsoleWidget* cw = con_store_get_widget(st->store, idx);
                ConItemId wid = con_store_get_id(st->store, idx);
                int is_active = (cw && st->active_for_user[e->user_id] == wid);
                int is_placeholder = (cw && st->lazy_mode != LAZY_OFF && !is_active) ? 1 : 0;
                /* Разрешаем drag либо для текстовых строк, либо для плейсхолдера */
                if (!cw || is_placeholder){
                    st->drag_arm = 1;
                    st->drag_start_mx = e->mouse.x;
                    st->drag_start_my = e->mouse.y;
                    st->drag_src_index = idx;
                    st->drag_is_placeholder = is_placeholder;
                    if (is_placeholder){
                        /* подготовлено выше в ветке обработчика — но если пришли сюда впервые, продублируем */
                        char tmp[128]; tmp[0]=0;
                        const char* t = cw && cw->as_text ? cw->as_text(cw, tmp, (int)sizeof(tmp)) : "[widget]";
                        SDL_strlcpy(st->drag_text_buf, t ? t : "[widget]", sizeof(st->drag_text_buf));
                        st->pending_placeholder_id = (st->lazy_mode==LAZY_TEXT_UNTIL_CLICK)? wid : 0;
                    } else {
                        st->pending_placeholder_id = 0;
                    }
                }
            }
        } else if (e->mouse.button==1 && e->mouse.state==0){
            /* отпускание — если не стартовали dnd, просто сбросить arm */
            if (st->drag_arm && st->drag_is_placeholder && st->pending_placeholder_id){
                /* Click без drag — активируем этот виджет в этой вьюхе для данного user */
                ConItemId prev = st->active_for_user[e->user_id];
                st->active_for_user[e->user_id] = st->pending_placeholder_id;
                invalidate_item_by_id(w, st, st->pending_placeholder_id);
                if (prev && prev != st->pending_placeholder_id) invalidate_item_by_id(w, st, prev);
            }
            st->drag_arm = 0;
            st->drag_src_index = -1;
            st->drag_is_placeholder = 0;
            st->pending_placeholder_id = 0;
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
                if (st->drag_is_placeholder){
                    line = st->drag_text_buf;
                    line_len = SDL_strlen(st->drag_text_buf);
                } else if (idx >= 0 && idx < con_store_count(st->store)){
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
                st->drag_is_placeholder = 0;
                st->pending_placeholder_id = 0;
                /* дальше событие будет обрабатываться протоколом DnD в input_route_mouse */
                return;
            }
        }
    }
}



/* ---------- vtable и init ---------- */

/* ---- Drag&Drop: разрешаем DROP некоторых типов для вставки интерактива ---- */

static void con_drag_enter(Window* w, const WMDrag* d){ (void)w;(void)d; }
static void con_drag_leave(Window* w, const WMDrag* d){ (void)w; (void)d; }
static void con_drag_over(Window* w, WMDrag* d, int lx, int ly){
    (void)lx;
    if (!d || !d->mime) { d->effect = WM_DRAG_NONE; return; }
    ConsoleViewState* st = (ConsoleViewState*)w->user;
    int H = surface_h(w->cache);
    int in_bot_prompt = (ly >= H - st->bot_h && ly < H);
    if (in_bot_prompt){
        if (strcmp(d->mime, "application/x-square")==0 ||
            strcmp(d->mime, MIME_CMD_TEXT)==0){
            d->effect = WM_DRAG_COPY;
        } else {
            d->effect = WM_DRAG_REJECT;
        }
        return;
    }
    if (strcmp(d->mime, "application/x-square")==0){
        d->effect = WM_DRAG_COPY; /* покажем, что можем вставить в консоль */
    } else if (strcmp(d->mime, MIME_CMD_TEXT)==0){
        d->effect = WM_DRAG_COPY;
    } else {
        d->effect = WM_DRAG_REJECT;
    }
}

static void con_drop(Window* w, WMDrag* d, int lx, int ly){
    (void)w;
    if (!d || !d->mime) return;
    ConsoleViewState *st = (ConsoleViewState*)w->user;
    int H = surface_h(w->cache);
    int in_bot_prompt = (ly >= H - st->bot_h && ly < H);
    if (in_bot_prompt){
        ConsolePrompt* tgt = st->prompt;
        if (!tgt) { d->effect = WM_DRAG_REJECT; return; }
        if (strcmp(d->mime, "application/x-square")==0){
            /* превратим квадрат в s-expr и положим в буфер промпта (делает con_prompt_on_drop) */
            con_prompt_on_drop(tgt, d->mime, d->data, d->size);
            d->effect = WM_DRAG_COPY; w->invalid_all = true; return;
        } else if (strcmp(d->mime, MIME_CMD_TEXT)==0){
            con_prompt_on_drop(tgt, d->mime, d->data, d->size);
            d->effect = WM_DRAG_COPY; w->invalid_all = true; return;
        }
        d->effect = WM_DRAG_REJECT; return;
    }

    if (strcmp(d->mime, "application/x-square")==0){
        /* Всегда вставляем виджет в КОНЕЦ ленты, игнорируя позицию дропа */
        (void)lx; (void)ly;
        uint8_t initial = 128;
        if (d->size >= (int)sizeof(SquarePayload) && d->data){
            const SquarePayload* sp = (const SquarePayload*)d->data;
            initial = (uint8_t)((sp->colA >> 16) & 0xFF);
        }
        if (st->sink){
            con_sink_insert_widget_color(st->sink, d->user_id, initial);
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


void win_console_set_lazy_mode(Window* w, LazyMode mode){
    if (!w || !w->user) return;
    ConsoleViewState* st = (ConsoleViewState*)w->user;
    st->lazy_mode = mode; w->invalid_all = true;
}

void win_console_init(Window *w, WM* wm, Rect frame, int z, ConsoleStore* store, ConsoleProcessor* proc, ConsoleSink* sink, int prompt_user_id){
    window_init(w, "console", frame, z, &V);

    ConsoleViewState *st = (ConsoleViewState*)calloc(1, sizeof(ConsoleViewState));
    st->col_bg = 0xFF000000;
    st->col_fg = 0xFFFFFFFF;

    console_measure(st, surface_w(w->cache), surface_h(w->cache));

    /* высота нижнего промпта по шрифту */
    int wM=0,hM=0; text_measure_utf8("M",&wM,&hM);
    int pad=6; st->top_h = 0; st->bot_h = hM + pad*2;
    int middle_h = surface_h(w->cache) - st->bot_h;
    if (middle_h < st->cell_h) middle_h = st->cell_h;
    st->rows = middle_h / st->cell_h;

    /* модель и sink */
    st->wm    = wm;
    st->store = store;
    st->proc  = proc;
    st->sink  = sink;
    /* подписка на изменения Store — чтобы вторая вьюха увидела обновления */
    con_store_subscribe(store, on_store_changed, w);

    /* промпт для конкретного пользователя внизу */
    st->prompt_user_id = prompt_user_id;
    st->prompt = con_prompt_create(prompt_user_id, sink, store);
    /* цвета промптов можно оставить дефолтными — бордеры рисуем сами */

    /* Lazy: по умолчанию выключен, карты активностей/аккордов — нули */
    st->lazy_mode = LAZY_OFF;
    for (int i=0;i<WM_MAX_USERS;i++){ st->active_for_user[i]=0; st->chord_stage[i]=0; }
    st->drag_is_placeholder = 0;
    st->pending_placeholder_id = 0;
    st->drag_arm = 0;
    st->drag_src_index = -1;

    st->dirty_rows_mask = 0;

    st->layout_valid = 0;
    st->layout_rows = 0;
    st->layout_start_index = 0;
    st->request_full_redraw = 1;

    w->user = st;
    w->animating = false;
    w->invalid_all = true;
}
