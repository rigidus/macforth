#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>

#define clampi(v,lo,hi) ((v)<(lo)?(lo):((v)>(hi)?(hi):(v)))

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#ifndef __EMSCRIPTEN__
extern int add_one(int x);
int asm_sum_output_size(void *win);
#endif

/* тайминг */
static inline uint32_t now_ms(void){ return SDL_GetTicks(); }
#define FRAME_MS 16u /* ~60 Hz */


void get_output_size(SDL_Window *win, int *out_w, int *out_h) {
    SDL_Surface *s = SDL_GetWindowSurface(win);
    if (s) {
        if (out_w) *out_w = s->w;
        if (out_h) *out_h = s->h;
    } else {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
    }
}

/* === Рисование в произвольный 32-битный surface (АРGB8888 backbuffer) === */
static inline void fill_surface32(SDL_Surface *surf, uint32_t color) {
    int pitch_px = surf->pitch / 4;
    uint32_t *base = (uint32_t*)surf->pixels;
    for (int y = 0; y < surf->h; ++y) {
        uint32_t *row = base + y * pitch_px;
        for (int x = 0; x < surf->w; ++x) row[x] = color;
    }
}

static inline void rect_translate(SDL_Rect* r,int dx,int dy){ r->x+=dx; r->y+=dy; }
static inline SDL_Rect rect_intersect(SDL_Rect a, SDL_Rect b){
    int x0= (a.x>b.x)?a.x:b.x, y0=(a.y>b.y)?a.y:b.y, x1=(a.x+a.w<b.x+b.w)?a.x+a.w:b.x+b.w, y1=(a.y+a.h<b.y+b.h)?a.y+a.h:b.y+b.h; SDL_Rect r={x0,y0, x1-x0, y1-y0}; if(r.w<0)r.w=0; if(r.h<0)r.h=0; return r;
}


/* процедурная заливка "шахматка" в ARGB32 surface */
static void fill_checkerboard(SDL_Surface* s, int tile, uint32_t c0, uint32_t c1){
    if (!s || tile <= 0) return;
    int pitch_px = s->pitch / 4;
    uint32_t* base = (uint32_t*)s->pixels;
    for (int y = 0; y < s->h; ++y){
        uint32_t* row = base + y * pitch_px;
        int by = (y / tile) & 1;
        for (int x = 0; x < s->w; ++x){
            int bx = (x / tile) & 1;
            row[x] = (bx ^ by) ? c0 : c1;
        }
    }
    /* тонкая сетка на границах тайлов (опционально, закомментируй если не нужно) */
    for (int y = 0; y < s->h; ++y){
        if (y % tile == 0) memset((uint8_t*)s->pixels + y*s->pitch, 0x20, s->w*4); /* тёмная линия */
    }
    for (int x = 0; x < s->w; ++x){
        if (x % tile == 0) for (int y=0; y<s->h; ++y) ((uint32_t*)s->pixels)[y*pitch_px + x] = 0xFF202020;
    }
}


/* заливка прямоугольника на ARGB32 surface */
static inline void fill_rect32(SDL_Surface* s, SDL_Rect r, uint32_t color){
    if (!s) return;
    SDL_Rect clip = rect_intersect(r, (SDL_Rect){0,0,s->w,s->h});
    if (clip.w<=0 || clip.h<=0) return;
    int pitch_px = s->pitch/4;
    uint32_t* base = (uint32_t*)s->pixels;
    for (int y=0; y<clip.h; ++y){ uint32_t* row = base + (clip.y+y)*pitch_px; for (int x=0; x<clip.w; ++x) row[clip.x+x]=color; }
}

static inline SDL_Rect rect_make(int x,int y,int w,int h){ SDL_Rect r={x,y,w,h}; return r; }

static int point_in_rect(int x, int y, const SDL_Rect *r) {
    return (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h);
}

// Глобальные/статические, чтобы к ним был доступ из sdl_tick():
static SDL_Window *g_win = NULL;
static SDL_Surface *g_surf = NULL;
/* backbuffer: всегда ARGB8888, рисуем сюда, потом blit в окно */
static SDL_Surface *g_back = NULL;
/* Текст */
static TTF_Font *g_font = NULL;         /* загружается из assets */
static int g_font_px = 18;              /* размер шрифта */
/* Состояние */
static int g_running = 1;
static bool g_animating = false;        /* есть ли активные анимации в каких-то окнах */
static uint32_t g_last_present_ms = 0;  /* кадрирование ≤ 60 Гц */
/* Damage (грязные прямоугольники на экране) */
#define MAX_DAMAGE 64
typedef struct { SDL_Rect r[MAX_DAMAGE]; int n; } DamageList;
static DamageList g_damage = { .n = 0 };
static void damage_clear() { g_damage.n = 0; }
static void damage_add(SDL_Rect rc) {
    if (rc.w<=0 || rc.h<=0) return;
    if (g_damage.n < MAX_DAMAGE) { g_damage.r[g_damage.n++] = rc; }
}
/* Цвета backbuffer’а в ARGB8888 (0xAARRGGBB) */
static const uint32_t g_col_bg     = 0xFF000000; /* чёрный */
static const uint32_t g_col_paint  = 0xFFFFFFFF; /* белый */
/* Размер квадрата */
static const int SQUARE_SIDE = 120;
/* ============================= WM / окна ============================= */
typedef struct Window Window;
typedef struct {
    int user_id; Window* focused; /* фокус как отдельная сущность, привязанная к пользователю */
} FocusEntry;
typedef struct {
    int dragging; int dx, dy; /* задел под перетаскивание */
} DragState;
struct Window {
    char     name[32];
    SDL_Rect frame;          /* позиция/размер в координатах экрана */
    int      zindex;         /* порядок наложения */
    bool     visible;
    bool     animating;      /* окно хочет 60 FPS (есть таймеры/эффекты) */
    SDL_Surface* cache;      /* пер-окошечный backbuffer ARGB8888 */
    bool     invalid_all;    /* нужно полностью перерисовать cache */
    void   (*draw)(Window* w, SDL_Rect area); /* перерисовать область окна в cache */
    void   (*on_event)(Window* w, const SDL_Event* e, int user_id, int local_x, int local_y);
    void   (*tick)(Window* w, uint32_t tnow); /* тик анимации/таймеров */
    DragState drag;
    uint32_t next_anim_ms;   /* ближайшее время тика анимации */
};
#define MAX_WINDOWS 8
static Window* g_windows[MAX_WINDOWS]; static int g_win_count=0;
static FocusEntry g_focus[4] = { {0,NULL} }; /* пока один пользователь: id=0 */


/* Создать/пересоздать backbuffer ARGB8888 под размеры окна */
static int ensure_backbuffer(int w, int h) {
    if (g_back && (g_back->w == w) && (g_back->h == h)) return 1;
    if (g_back) { SDL_FreeSurface(g_back); g_back = NULL; }
    g_back = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!g_back) {
        SDL_Log("Create backbuffer failed: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

/* --------------------- API: управление окнами --------------------- */
static SDL_Surface* make_surface_argb(int w,int h){
    return SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_ARGB8888);
}
static void window_invalidate(Window* w, SDL_Rect area_screen){
    /* помечаем окно и добавляем damage на экран */
    w->invalid_all = true; /* упрощённо: перерисуем окно целиком; можно доработать частичные */
    damage_add(area_screen);
}
static void wm_add_window(Window* w){
    if (g_win_count>=MAX_WINDOWS) return;
    g_windows[g_win_count++] = w;
    /* сортировку по zindex можно делать по мере добавления (вставкой) — для простоты пересортируем целиком */
}
static void wm_remove_window(Window* w){
    for(int i=0;i<g_win_count;i++) if (g_windows[i]==w){
            for(int j=i+1;j<g_win_count;j++) g_windows[j-1]=g_windows[j];
            g_win_count--; break;
        }
}
/* найти максимальный zindex среди окон */
static int wm_max_z(void){
    int mz = (g_win_count>0) ? g_windows[0]->zindex : 0;
    for (int i=1;i<g_win_count;i++){
        if (g_windows[i]->zindex > mz) mz = g_windows[i]->zindex;
    }
    return mz;
}
static void wm_sort_by_z(){
    /* простая сортировка по zindex */
    for(int i=0;i<g_win_count;i++) for(int j=i+1;j<g_win_count;j++) {
            if (g_windows[i]->zindex > g_windows[j]->zindex){
                Window* t=g_windows[i]; g_windows[i]=g_windows[j]; g_windows[j]=t;
            }
        }
}
/* поднять окно на передний план (делает его самым верхним) */
static void wm_bring_to_front(Window* w){
    if (!w) return;
    int newZ = wm_max_z() + 1;
    if (w->zindex == newZ) return;
    w->zindex = newZ;
    wm_sort_by_z();                 /* пересортировать стек */
    damage_add(w->frame);           /* перекомпозировать его область */
    w->invalid_all = true;          /* на всякий случай перерисовать кэш */
}
static Window* wm_topmost_at(int x,int y){
    /* сверху вниз: больший zindex — выше; в нашем массиве будем сканировать с конца */
    if (g_win_count==0) return NULL;
    int best=-1; int bestZ=-2147483647;
    for(int i=0;i<g_win_count;i++){
        Window* w=g_windows[i];
        if(!w->visible) continue;
        SDL_Rect f=w->frame;
        if(point_in_rect(x,y,&f) && w->zindex>=bestZ){ best=i; bestZ=w->zindex; }
    }
    return (best>=0)?g_windows[best]:NULL;
}
static void wm_resize(int newW,int newH){
    /* пересоздаём g_back, инвалидация всего экрана */
    ensure_backbuffer(newW,newH);
    damage_add((SDL_Rect){0,0,newW,newH});
    /* пересоздаём cache у окон, завязанных на размер (в демо: окно_1 = фоновое) */
    for (int i=0;i<g_win_count;i++){
        Window* w = g_windows[i];
        /* эвристика: если окно покрывает весь экран, считаем его «фон» */
        if (w->frame.x==0 && w->frame.y==0 && w->frame.w!=newW){
            w->frame.w=newW; w->frame.h=newH;
            if (w->cache){ SDL_FreeSurface(w->cache); }
            w->cache = make_surface_argb(w->frame.w, w->frame.h);
            w->invalid_all = true;
        }
    }
}

/* вычисляем, есть ли активные анимации (для pacing) */
static bool wm_any_animating(void){
    for (int i=0;i<g_win_count;i++){
        if (g_windows[i]->visible && g_windows[i]->animating) return true;
    }
    return false;
}

/* обновление анимаций окон (например, мигание) — только отметка damage/invalid, без композиции */
static void wm_tick_animations(uint32_t tnow){
    for (int i=0;i<g_win_count;i++){
        Window* w = g_windows[i];
        if (!w->visible || !w->animating) continue;
        if (tnow >= w->next_anim_ms){
            if (w->tick) w->tick(w, tnow); /* сам tick пометит invalid и damage */
        }
    }
}

/* --------------------- ОКНА: реализация демо --------------------- */
/* окно_1 (фон): рисуем точки мышью поверх собственного cache */
typedef struct {
    Window base;
    /* своё состояние */
} PaintWindow;
static void draw_window1(Window* w, SDL_Rect area){
    /* ничего не очищаем: cache хранит «накопленный» рисунок */
    (void)area;
    w->invalid_all = false;
}
static void on_event_window1(Window* w, const SDL_Event* e, int user_id, int lx, int ly){
    (void)user_id;
    if (e->type==SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT){
        /* первый клик: поставить точку */
        /* локальные координаты lx,ly гарантированно внутри окна */
        int pitch_px = w->cache->pitch/4;
        if ((unsigned)lx < (unsigned)w->cache->w && (unsigned)ly < (unsigned)w->cache->h){
            ((uint32_t*)w->cache->pixels)[ly*pitch_px+lx] = g_col_paint;
            SDL_Rect dr = { w->frame.x+lx, w->frame.y+ly, 1, 1 };
            window_invalidate(w, dr);
        }
    } else if (e->type==SDL_MOUSEMOTION && (e->motion.state & SDL_BUTTON_LMASK)){
        int pitch_px = w->cache->pitch/4;
        if ((unsigned)lx < (unsigned)w->cache->w && (unsigned)ly < (unsigned)w->cache->h){
            ((uint32_t*)w->cache->pixels)[ly*pitch_px+lx] = g_col_paint;
            SDL_Rect dr = { w->frame.x+lx, w->frame.y+ly, 1, 1 };
            window_invalidate(w, dr);
        }
    }
}

/* окно_2: квадрат, который меняет цвет по клику; всегда перерисовывает свой cache целиком (маленькое окно) */
typedef struct {
    Window base;
    uint32_t color;       /* текущий цвет */
    uint32_t start_ms;    /* начало анимации */
    uint32_t period_ms;   /* полный цикл анимации, мс (туда-обратно) */
    float    phase_bias;   /* фазовый сдвиг в [0,1); клики добавляют +0.5 для разворота */
    uint32_t colA, colB;   /* ПАРА цветов для интерполяции */
} SquareWindow;
static void draw_square_into(SDL_Surface* s, uint32_t color){
    /* центруем квадрат в пределах s */
    SDL_Rect cs; cs.w=SQUARE_SIDE; cs.h=SQUARE_SIDE; cs.x=(s->w-SQUARE_SIDE)/2; cs.y=(s->h-SQUARE_SIDE)/2;
    /* очистка фона окна_2 (прозрачный фон не используем — просто чёрный подложим для наглядности) */
    fill_surface32(s, 0xFF101010);
    int pitch_px = s->pitch/4; uint32_t* base=(uint32_t*)s->pixels;
    for (int yy=0; yy<cs.h; ++yy){
        int ry=cs.y+yy; if((unsigned)ry>=(unsigned)s->h) continue;
        uint32_t* row = base + ry*pitch_px;
        for (int xx=0; xx<cs.w; ++xx){
            int rx=cs.x+xx; if((unsigned)rx>=(unsigned)s->w) continue;
            row[rx] = color;
        }
    }
}
static void draw_window2(Window* w, SDL_Rect area){
    (void)area;
    SquareWindow* sw = (SquareWindow*)w;
    draw_square_into(w->cache, sw->color);
    w->invalid_all=false;
}
/* линейная интерполяция по каналам 0xAARRGGBB */
static inline uint32_t argb_lerp(uint32_t a, uint32_t b, float t){
#define CH(c,s) ((int)(((c)>>(s))&0xFF))
    int aA=CH(a,24), aR=CH(a,16), aG=CH(a,8), aB=CH(a,0);
    int bA=CH(b,24), bR=CH(b,16), bG=CH(b,8), bB=CH(b,0);
    int AA = (int)(aA + (bA-aA)*t);
    int RR = (int)(aR + (bR-aR)*t);
    int GG = (int)(aG + (bG-aG)*t);
    int BB = (int)(aB + (bB-aB)*t);
    return (uint32_t)((AA<<24)|(RR<<16)|(GG<<8)|BB);
#undef CH
}
static void tick_window2(Window* w, uint32_t tnow){
    SquareWindow* sw = (SquareWindow*)w;
    uint32_t period = sw->period_ms ? sw->period_ms : 2000;
    uint32_t elapsed = tnow - sw->start_ms;
    float u = (period>0)? (float)(elapsed % period) / (float)period : 0.0f; /* 0..1 */
    /* фазовый сдвиг от кликов */
    u = u + sw->phase_bias; u = u - floorf(u);
    /* косинусное сглаживание: туда-обратно */
    float m = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * u);
    sw->color = argb_lerp(sw->colA, sw->colB, m);
    /* просим перерисовать окно и планируем следующий тик через кадр (~60 Гц) */
    w->invalid_all = true;
    damage_add(w->frame);
    w->next_anim_ms = tnow + FRAME_MS;
}
static void on_event_window2(Window* w, const SDL_Event* e, int user_id, int lx, int ly){
    (void)user_id;
    if (e->type==SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT){
        /* Попали в квадрат? — смена цвета; иначе начинаем перетаскивание */
        SDL_Rect cs = { (w->cache->w - SQUARE_SIDE)/2, (w->cache->h - SQUARE_SIDE)/2, SQUARE_SIDE, SQUARE_SIDE };
        if (point_in_rect(lx,ly,&cs)){
            SquareWindow* sw = (SquareWindow*)w;
            /* заметная реакция: разворачиваем фазу на полцикла */
            sw->phase_bias += 0.5f; if (sw->phase_bias >= 1.0f) sw->phase_bias -= 1.0f;
            tick_window2(w, now_ms()); /* мгновенный пересчёт кадра */
            w->invalid_all = true;
            window_invalidate(w, w->frame);
            printf("Square clicked at (%d,%d)\n", lx, ly); fflush(stdout);
        } else {
            w->drag.dragging = 1; w->drag.dx = lx; w->drag.dy = ly;
        }
    } else if (e->type==SDL_MOUSEMOTION && (e->motion.state & SDL_BUTTON_LMASK) && w->drag.dragging){
        /* Перетаскивание: damage старого и нового положения */
        SDL_Rect old = w->frame;
        int nx = w->frame.x + (lx - w->drag.dx);
        int ny = w->frame.y + (ly - w->drag.dy);
        /* ограничим в пределах экрана */
        nx = clampi(nx, 0, g_surf ? (g_surf->w - w->frame.w) : nx);
        ny = clampi(ny, 0, g_surf ? (g_surf->h - w->frame.h) : ny);
        if (nx!=w->frame.x || ny!=w->frame.y){
            w->frame.x = nx; w->frame.y = ny;
            w->invalid_all = true; /* окно надо перерисовать (минимум фон) */
            damage_add(old); damage_add(w->frame);
        }
    } else if (e->type==SDL_MOUSEBUTTONUP && e->button.button==SDL_BUTTON_LEFT){
        w->drag.dragging = 0;
    }
}

/* фабрики окон */
static void init_window(Window* w,const char* name, SDL_Rect frame, int z, void(*draw)(Window*,SDL_Rect), void(*on_ev)(Window*,const SDL_Event*,int,int,int)){
    memset(w,0,sizeof(*w));
    snprintf(w->name,sizeof(w->name),"%s",name);
    w->frame=frame; w->zindex=z; w->visible=true; w->animating=false; w->invalid_all=true;
    w->draw=draw; w->on_event=on_ev;
    w->cache = make_surface_argb(frame.w, frame.h);
    /* фоновые окна удобно один раз залить фоном (иначе неопределённые пиксели) */
    if (w->cache) fill_surface32(w->cache, g_col_bg);
}



/* создать surface с текстом (RGBA8), вызывающий должен SDL_FreeSurface */
static SDL_Surface* text_to_surface(const char *utf8, uint32_t rgba)
{
    if (!g_font || !utf8) return NULL;
    SDL_Color col = {
        .r = (rgba >> 16) & 0xFF,
        .g = (rgba >>  8) & 0xFF,
        .b = (rgba >>  0) & 0xFF,
        .a = (rgba >> 24) & 0xFF
    };
    /* Blended даёт сглаженный текст с альфой */
    SDL_Surface *s = TTF_RenderUTF8_Blended(g_font, utf8, col);
    return s; /* может быть не ARGB8888 */
}

/* нарисовать текст в backbuffer по (x,y); цвет ARGB */
static void draw_text(int x, int y, const char *utf8, uint32_t argb)
{
    if (!g_back || !utf8 || !*utf8) return;
    SDL_Surface *s = text_to_surface(utf8, /*RGBA*/ (argb << 8) | (argb >> 24));
    if (!s) return;
    /* при необходимости конвертируем в ARGB8888 */
    SDL_Surface *conv = s;
    if (s->format->format != SDL_PIXELFORMAT_ARGB8888) {
        conv = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
        SDL_FreeSurface(s);
        if (!conv) return;
    }
    SDL_Rect dst = { x, y, conv->w, conv->h };
    /* blit с учётом альфы */
    SDL_SetSurfaceBlendMode(conv, SDL_BLENDMODE_BLEND);
    SDL_BlitSurface(conv, NULL, g_back, &dst);
    SDL_FreeSurface(conv);
}

/* ======================= Композиция и показ ======================= */
static void compose_and_present_if_due(){
    /* если нет damage и нет активных анимаций — ничего не делаем */
    g_animating = wm_any_animating();
    if (g_damage.n==0 && !g_animating) return;
    uint32_t t = now_ms();
    /* тикаем анимации (они только помечают invalid и добавляют damage) */
    if (g_animating){
        wm_tick_animations(t);
        /* могли появиться новые damage внутри тика анимаций */
    }
    if (g_animating){
        if (t - g_last_present_ms < FRAME_MS) return; /* соблюдаем ≤60 FPS */
    }
    /* Композиция только по damage: для каждого прямоугольника — пройти окна снизу вверх и бличить видимые части */
    for(int di=0; di<g_damage.n; ++di){
        SDL_Rect dr = g_damage.r[di];
        /* на всякий случай ограничим damage экраном */
        dr = rect_intersect(dr, (SDL_Rect){0,0, g_back?g_back->w:0, g_back?g_back->h:0});
        if (dr.w<=0 || dr.h<=0) continue;
        /* ВАЖНО: очистить область кадра перед композицией, чтобы не оставались "следы" */
        fill_rect32(g_back, dr, g_col_bg);
        /* по окнам — в возрастающем zindex (снизу вверх) */
        for(int wi=0; wi<g_win_count; ++wi){
            Window* w = g_windows[wi]; if(!w->visible) continue;
            SDL_Rect inter = rect_intersect(dr, w->frame);
            if (inter.w<=0 || inter.h<=0) continue;
            /* если окно требовало перерисовку — обновить его cache прежде чем композитить */
            if (w->invalid_all) {
                w->draw(w, w->frame); /* упрощённо: перерисуй всё окно */
            }
            /* источник в локальных координатах окна */
            SDL_Rect src = inter; rect_translate(&src, -w->frame.x, -w->frame.y);
            SDL_BlitSurface(w->cache, &src, g_back, &inter);
        }
    }
    /* СКИНУТЬ кадр в surface окна: g_back -> g_surf, только dirty-области */
    if (g_surf) {
        for (int di=0; di<g_damage.n; ++di) {
            SDL_Rect dr = g_damage.r[di];
            SDL_BlitSurface(g_back, &dr, g_surf, &dr);
        }
        /* показать только повреждённые области */
        SDL_UpdateWindowSurfaceRects(g_win, g_damage.r, g_damage.n);
    }
    g_last_present_ms = t;
    damage_clear();
}

static void sdl_tick(void) {
    SDL_Event e;
    bool had_events = false;
    while (SDL_PollEvent(&e)) {
        had_events = true;
        switch (e.type) {
        case SDL_QUIT: 
            g_running = 0; 
#ifdef __EMSCRIPTEN__
            emscripten_cancel_main_loop();
#endif
            break;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_ESCAPE) {
                g_running = 0;
#ifdef __EMSCRIPTEN__
                emscripten_cancel_main_loop();
#endif
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEMOTION:
        {
            /* мультипользовательский сценарий — выберем user_id=0 */
            int uid = 0;
            /* хит-тест — верхнее видимое окно под курсором */
            int mx = (e.type==SDL_MOUSEMOTION)? e.motion.x : e.button.x;
            int my = (e.type==SDL_MOUSEMOTION)? e.motion.y : e.button.y;
            Window* top = wm_topmost_at(mx,my);
            if (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT){
                /* поменять фокус пользователя на top (или на NULL, если клик по пустому месту) */
                if (top){
                    /* поднять на передний план окно, которое получило фокус */
                    wm_bring_to_front(top);
                }
                g_focus[uid].user_id = uid;
                g_focus[uid].focused = top;
                /* если кликнули по пустому месту — можно снять фокус */
                if (!top){
                    /* опционально: damage всего экрана не нужен, стек не менялся */
                }
            }
            /* маршрутизация ввода: только в сфокусированное для этого пользователя окно */
            Window* target = g_focus[uid].focused;
            if (target){
                /* преобразуем координаты в локальные */
                int lx = mx - target->frame.x;
                int ly = my - target->frame.y;
                if (target->on_event) target->on_event(target, &e, uid, lx, ly);
                /* on_event сам вызывает window_invalidate при изменениях */
            }
        } break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                g_surf = SDL_GetWindowSurface(g_win);
                if (!g_surf) break;
                wm_resize(g_surf->w, g_surf->h);
                // Сообщим новый размер области вывода
                int ow, oh;
                get_output_size(g_win, &ow, &oh);
                printf("Размер области вывода изменён: %d x %d\n", ow, oh);
                fflush(stdout);
            } else if (e.window.event == SDL_WINDOWEVENT_EXPOSED) {
                /* просто показать текущий g_back, без перерисовки */
                if (g_surf) {
                    if (g_damage.n==0) damage_add((SDL_Rect){0,0,g_surf->w,g_surf->h});
                }
            }
            break;  
        }        
    } // while(SDL_PollEvent)

    /* кадр: композим/анимируем даже при отсутствии событий */
    compose_and_present_if_due();

#ifndef __EMSCRIPTEN__
    /* Пейсинг: если нет анимаций и damage — слегка поспать; если анимация есть — подровнять до ~60 Гц */
    if (g_damage.n==0 && !wm_any_animating()) {
        SDL_Delay(8);
    } else {
        static uint32_t prev = 0;
        uint32_t t = SDL_GetTicks();
        if (prev && (t - prev) < FRAME_MS) SDL_Delay(FRAME_MS - (t - prev));
        prev = t;
    }
#endif
}
    
int main(int argc, char **argv) {
    (void)argc; (void)argv;

#ifndef __EMSCRIPTEN__
    printf("%d\n", add_one(41)); // 42
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init error: %s", SDL_GetError());
        return 1;
    }

    /* Инициализация SDL_ttf */
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init error: %s", TTF_GetError());
        return 1;
    }
    /* Загружаем **моноширинный** шрифт DejaVu Sans Mono.
       - desktop: ./assets/DejaVuSansMono.ttf
       - web:     /assets/DejaVuSansMono.ttf (через --preload-file) */
    const char *font_path =
#ifdef __EMSCRIPTEN__
        "/assets/DejaVuSansMono.ttf";
#else
        "assets/DejaVuSansMono.ttf";
#endif
    g_font = TTF_OpenFont(font_path, g_font_px);
    if (!g_font) {
        SDL_Log("TTF_OpenFont('%s') error: %s", font_path, TTF_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    TTF_SetFontHinting(g_font, TTF_HINTING_LIGHT);

    int w = 800, h = 600;
    SDL_Window *win = SDL_CreateWindow(
        "SDL Points Simple", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { SDL_Log("CreateWindow: %s", SDL_GetError()); SDL_Quit(); return 1; }

    g_win = win;
    g_surf = SDL_GetWindowSurface(g_win);
    if (!g_surf) { SDL_Log("GetWindowSurface: %s", SDL_GetError()); SDL_DestroyWindow(g_win); SDL_Quit(); return 1; }

    /* Создаём backbuffer под размер окна */
    if (!ensure_backbuffer(g_surf->w, g_surf->h)) {
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return 1;
    }
    

    /* ====== Создаём два внутренних окна ====== */
    /* окно_1: на весь экран, з=0 */
    static PaintWindow win1;
    init_window((Window*)&win1, "window_1", (SDL_Rect){0,0,g_surf->w,g_surf->h}, 0, draw_window1, on_event_window1);
    /* окно_1: зальём орнаментом (шахматка). Цвета — лёгкий тёмный паттерн. */
    {
        Window* w = (Window*)&win1;
        if (w->cache){
            /* цвета ARGB */
            uint32_t c0 = 0xFF181818;
            uint32_t c1 = 0xFF101010;
            fill_checkerboard(w->cache, 16, c0, c1);
        }
    }
    wm_add_window((Window*)&win1);

    /* окно_2: сверху, 360x240, по центру; з=1 */
    static SquareWindow win2;
    int w2=360,h2=240, x2=(g_surf->w - w2)/2, y2=(g_surf->h - h2)/2;
    init_window((Window*)&win2, "window_2", (SDL_Rect){x2,y2,w2,h2}, 1, draw_window2, on_event_window2);
    /* цвета wnd2 — прежняя пара (красный <-> зелёный) */
    win2.colA      = 0xFFFF0000;            /* красный  */
    win2.colB      = 0xFF00FF00;            /* зелёный  */
    win2.color     = win2.colA;             /* стартовый цвет */
    win2.start_ms  = now_ms();              /* начало анимации */
    win2.period_ms = 2000;                  /* полный цикл 2.0 сек */
    win2.phase_bias = 0.0f;
    ((Window*)&win2)->tick = tick_window2;
    wm_add_window((Window*)&win2);

    /* окно_3: похоже на wnd2, но с ДРУГИМИ цветами; немного сдвинем вправо/вниз, z=2 */
    static SquareWindow win3;
    int w3=360,h3=240, x3=x2+40, y3=y2+40;
    init_window((Window*)&win3, "window_3", (SDL_Rect){x3,y3,w3,h3}, 2, draw_window2, on_event_window2);
    /* цвета wnd3 —, например, сине-розовый градиент */
    win3.colA       = 0xFF1E90FF;           /* DodgerBlue */
    win3.colB       = 0xFFFF66FF;           /* розово-лиловый */
    win3.color      = win3.colA;
    win3.start_ms   = now_ms();
    win3.period_ms  = 3000;                 /* отличная длительность для разнообразия */
    win3.phase_bias = 0.25f;                /* стартуем с фазовым сдвигом */
    ((Window*)&win3)->tick = tick_window2;
    wm_add_window((Window*)&win3);
    
    wm_sort_by_z();

    /* начальный damage всего экрана, чтобы собрать первый кадр */
    /* g_back будет собран из внутренних окон; можно не заливать его дважды */
    fill_surface32(g_back, g_col_bg);
    damage_add(rect_make(0,0,g_surf->w,g_surf->h));

    /* включим плавную анимацию цвета для обоих окон */
    ((Window*)&win2)->animating    = true; ((Window*)&win2)->next_anim_ms = now_ms();
    ((Window*)&win3)->animating    = true; ((Window*)&win3)->next_anim_ms = now_ms();

#ifndef __EMSCRIPTEN__
    int sum = asm_sum_output_size(win);
    printf("w + h = %d\n", sum);
#endif

    compose_and_present_if_due();
    int ow = 0, oh = 0; get_output_size(win, &ow, &oh);
    printf("Размер области вывода: %d x %d\n", ow, oh); fflush(stdout);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(sdl_tick, 0, 1);
    // сюда не вернёмся; emscripten сам держит цикл
#else
    while (g_running) {
        sdl_tick();
    }
#endif

    /* уничтожение окон */
    for (int i=0;i<g_win_count;i++){
        if (g_windows[i] && g_windows[i]->cache){ SDL_FreeSurface(g_windows[i]->cache); g_windows[i]->cache=NULL; }
        g_windows[i]=NULL;
    }
    g_win_count=0;
    if (g_back) { SDL_FreeSurface(g_back); g_back = NULL; }

    SDL_DestroyWindow(g_win);

    if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
    TTF_Quit();
    SDL_Quit();
    return 0;
}
