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

static inline void put_pixel32(uint32_t *pixels, int pitch_pixels, int x, int y, uint32_t px) {
    pixels[y * pitch_pixels + x] = px;
}

static void draw_filled_rect(SDL_Surface *surf, int x, int y, int w, int h, uint32_t color) {
    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
    int pitch_px = surf->pitch / 4;
    for (int yy = 0; yy < h; ++yy) {
        int ry = y + yy;
        if (ry < 0 || ry >= surf->h) continue;
        uint32_t *row = (uint32_t*)surf->pixels + ry * pitch_px;
        for (int xx = 0; xx < w; ++xx) {
            int rx = x + xx;
            if (rx < 0 || rx >= surf->w) continue;
            row[rx] = color;
        }
    }
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
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
static int g_running = 1;
static int g_mouse_down = 0;
static uint32_t g_bg = 0, g_paint = 0;
static uint32_t g_button = 0; // цвет квадрата
static const int SQUARE_SIDE = 120;
static SDL_Rect g_square = NULL;


static void sdl_redraw_static_ui(void) {
    // если у тебя есть квадрат/кнопка по центру — перерисуй его тут
    center_square_rect(g_surf, SQUARE_SIDE, &g_square);
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
                        printf("Square clicked at (%d, %d)\\n",
                               e.button.x, e.button.y);
                        fflush(stdout);
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
                    if (SDL_MUSTLOCK(g_surf)) SDL_LockSurface(g_surf);
                    // поставим «точку»
                    int pitch_px = g_surf->pitch / 4;
                    put_pixel32((uint32_t*)g_surf->pixels, pitch_px, e.motion.x, e.motion.y, g_paint);
                    if (SDL_MUSTLOCK(g_surf)) SDL_UnlockSurface(g_surf);
                    
                    // Перерисуем квадрат поверх, чтобы он оставался видимым
                    draw_filled_rect(g_surf, g_square.x, g_square.y, g_square.w, g_square.h, g_button);
                    sdl_redraw_static_ui();
                    SDL_UpdateWindowSurface(g_win);
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    g_surf = SDL_GetWindowSurface(g_win);
                    g_bg    = SDL_MapRGB(g_surf->format, 0, 0, 0);
                    g_paint = SDL_MapRGB(g_surf->format, 255, 255, 255);
                    g_button = SDL_MapRGB(g_surf->format, 255, 0, 0);
                    center_square_rect(g_surf, SQUARE_SIDE, &g_square);
                    // Обновим экран: просто перерисуем квадрат поверх текущего содержимого
                    draw_filled_rect(g_surf, g_square.x, g_square.y, g_square.w, g_square.h, g_button);
                    // Сообщим новый размер области вывода
                    int ow, oh;
                    get_output_size(g_win, &ow, &oh);
                    printf("Размер области вывода изменён: %d x %d\n", ow, oh);
                    fflush(stdout);
                    sdl_redraw_static_ui();
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

    g_bg    = SDL_MapRGB(g_surf->format, 0, 0, 0);
    g_paint = SDL_MapRGB(g_surf->format, 255, 255, 255);
    g_button = SDL_MapRGB(g_surf->format, 255, 0, 0); // цвет квадрата

    int ow = 0, oh = 0;
    get_output_size(win, &ow, &oh);
    printf("Размер области вывода:: %d x %d\n", ow, oh);
    fflush(stdout);


    // Очистка фона
    if (SDL_MUSTLOCK(g_surf)) SDL_LockSurface(g_surf);
    memset(g_surf->pixels, 0, (size_t)g_surf->h * g_surf->pitch);
    if (SDL_MUSTLOCK(g_surf)) SDL_UnlockSurface(g_surf);
    SDL_UpdateWindowSurface(g_win);

    center_square_rect(g_surf, SQUARE_SIDE, &g_square);
    draw_filled_rect(g_surf, g_square.x, g_square.y, g_square.w, g_square.h, g_button);

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

    SDL_DestroyWindow(g_win);

    SDL_Quit();
    return 0;
}
