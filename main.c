#include <SDL.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#ifndef __EMSCRIPTEN__
extern int add_one(int x);
int asm_sum_output_size(void *win);
#endif

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

static inline void draw_filled_rect32(SDL_Surface *surf, int x, int y, int w, int h, uint32_t color) {
    int pitch_px = surf->pitch / 4;
    uint32_t *base = (uint32_t*)surf->pixels;
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = x + w; if (x1 > surf->w) x1 = surf->w;
    int y1 = y + h; if (y1 > surf->h) y1 = surf->h;
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t *row = base + yy * pitch_px;
        for (int xx = x0; xx < x1; ++xx) row[xx] = color;
    }
}

static void center_square_rect(SDL_Surface *surf, int side, SDL_Rect *out) {
    out->w = side; out->h = side;
    out->x = (surf->w - side) / 2;
    out->y = (surf->h - side) / 2;
}

static int point_in_rect(int x, int y, const SDL_Rect *r) {
    return (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h);
}

// Глобальные/статические, чтобы к ним был доступ из sdl_tick():
static SDL_Window *g_win = NULL;
static SDL_Surface *g_surf = NULL;
/* backbuffer: всегда ARGB8888, рисуем сюда, потом blit в окно */
static SDL_Surface *g_back = NULL;
static int g_running = 1;
static int g_mouse_down = 0;
/* Цвета backbuffer’а в ARGB8888 (0xAARRGGBB) */
static const uint32_t g_col_bg     = 0xFF000000; /* чёрный */
static const uint32_t g_col_paint  = 0xFFFFFFFF; /* белый */
/* Квадрат будет переключаться между двумя цветами */
static const uint32_t g_col_button1 = 0xFFFF0000; /* красный */
static const uint32_t g_col_button2 = 0xFF00FF00; /* зелёный */
static uint32_t g_button_col = 0;                 /* текущий цвет квадрата */
/* Размер квадрата */
static const int SQUARE_SIDE = 120;
static SDL_Rect g_square;

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

/* Пересчитать геометрию UI (центр квадрата) */
static void recalc_ui_layout(void) {
    center_square_rect(g_back ? g_back : g_surf, SQUARE_SIDE, &g_square);
}

/* Нарисовать статический UI (кнопку) в backbuffer */
static void draw_static_ui(void) {
    if (!g_back) return;
    /* предполагается, что backbuffer уже залочен снаружи */
    draw_filled_rect32(g_back, g_square.x, g_square.y, g_square.w, g_square.h, g_button_col);
}


/* Безопасная запись точки в пределах backbuffer */
static inline void draw_point_safe(int x, int y) {
    if (!g_back) return;
    if ((unsigned)x < (unsigned)g_back->w && (unsigned)y < (unsigned)g_back->h) {
        int pitch_px = g_back->pitch / 4;
        ((uint32_t*)g_back->pixels)[y * pitch_px + x] = g_col_paint;
    }
}

static void sdl_tick(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: g_running = 0; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) g_running = 0;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (point_in_rect(e.button.x, e.button.y, &g_square)) {
                        printf("Square clicked at (%d, %d)\n",
                               e.button.x, e.button.y);
                        fflush(stdout);
                        /* Переключаем цвет квадрата и перерисовываем */
                        g_button_col = (g_button_col == g_col_button1) ? g_col_button2 : g_col_button1;
                        if (SDL_MUSTLOCK(g_back)) SDL_LockSurface(g_back);
                        /* Перерисуем только квадрат — фон/точки не трогаем */
                        draw_static_ui();
                        if (SDL_MUSTLOCK(g_back)) SDL_UnlockSurface(g_back);
                        SDL_BlitSurface(g_back, NULL, g_surf, NULL);
                        SDL_UpdateWindowSurface(g_win);
                    } else {
                        g_mouse_down = 1;
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) g_mouse_down = 0;
                break;
            case SDL_MOUSEMOTION:
                if (g_mouse_down) {
                    /* Рисуем весь кадр в backbuffer под одним lock */
                    if (SDL_MUSTLOCK(g_back)) SDL_LockSurface(g_back);
                    draw_point_safe(e.motion.x, e.motion.y);
                    draw_static_ui();
                    if (SDL_MUSTLOCK(g_back)) SDL_UnlockSurface(g_back);
                    /* Показать кадр: blit → окно */
                    SDL_BlitSurface(g_back, NULL, g_surf, NULL);
                    SDL_UpdateWindowSurface(g_win);
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    g_surf = SDL_GetWindowSurface(g_win);
                    if (!g_surf) break;
                    /* пересоздаём backbuffer под новый размер */
                    if (!ensure_backbuffer(g_surf->w, g_surf->h)) break;
                    recalc_ui_layout();
                    /* можно не очищать, но для предсказуемости очистим фон */
                    if (SDL_MUSTLOCK(g_back)) SDL_LockSurface(g_back);
                    fill_surface32(g_back, g_col_bg);
                    draw_static_ui();
                    if (SDL_MUSTLOCK(g_back)) SDL_UnlockSurface(g_back);
                    SDL_BlitSurface(g_back, NULL, g_surf, NULL);
                    // Сообщим новый размер области вывода
                    int ow, oh;
                    get_output_size(g_win, &ow, &oh);
                    printf("Размер области вывода изменён: %d x %d\n", ow, oh);
                    fflush(stdout);
                    SDL_UpdateWindowSurface(g_win);
                }
                break;
        }
    }
#ifndef __EMSCRIPTEN__
    SDL_Delay(1); // в браузере задержка не нужна: кадры идут по rAF
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
    
    int ow = 0, oh = 0;
    get_output_size(win, &ow, &oh);
    printf("Размер области вывода:: %d x %d\n", ow, oh);
    fflush(stdout);

    /* Инициализация сцены: центр квадрата и первый кадр в backbuffer */
    g_button_col = g_col_button1; /* стартовый цвет квадрата */
    recalc_ui_layout();
    if (SDL_MUSTLOCK(g_back)) SDL_LockSurface(g_back);
    fill_surface32(g_back, g_col_bg);
    draw_static_ui();
    if (SDL_MUSTLOCK(g_back)) SDL_UnlockSurface(g_back);
    SDL_BlitSurface(g_back, NULL, g_surf, NULL);

#ifndef __EMSCRIPTEN__
    int sum = asm_sum_output_size(win);
    printf("w + h = %d\n", sum);
#endif

    SDL_UpdateWindowSurface(g_win);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(sdl_tick, 0, 1);
    // сюда не вернёмся; emscripten сам держит цикл
#else
    while (g_running) {
        sdl_tick();
    }
#endif

    if (g_back) { SDL_FreeSurface(g_back); g_back = NULL; }
    SDL_DestroyWindow(g_win);

    SDL_Quit();
    return 0;
}
