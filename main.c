#include <SDL.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern int add_one(int x);

int asm_sum_output_size(void *win);

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

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("%d\n", add_one(41)); // 42

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init error: %s", SDL_GetError());
        return 1;
    }

    int w = 800, h = 600;
    SDL_Window *win = SDL_CreateWindow(
        "SDL Points Simple", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        w, h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { SDL_Log("CreateWindow: %s", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_Surface *surf = SDL_GetWindowSurface(win);
    if (!surf) { SDL_Log("GetWindowSurface: %s", SDL_GetError()); SDL_DestroyWindow(win); SDL_Quit(); return 1; }

    uint32_t bg = SDL_MapRGB(surf->format, 0, 0, 0);
    uint32_t paint = SDL_MapRGB(surf->format, 255, 255, 255);

    int ow = 0, oh = 0;
    get_output_size(win, &ow, &oh);
    printf("Размер области вывода: %d x %d\n", ow, oh);
    fflush(stdout);

    uint32_t button = SDL_MapRGB(surf->format, 255, 0, 0); // цвет квадрата
    const int SQUARE_SIDE = 120;
    SDL_Rect square;
    center_square_rect(surf, SQUARE_SIDE, &square);

    // Очистка фона
    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
    memset(surf->pixels, 0, (size_t)surf->h * surf->pitch);
    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);

    draw_filled_rect(surf, square.x, square.y, square.w, square.h, button);

    int sum = asm_sum_output_size(win);
    printf("w + h = %d\n", sum);

    SDL_UpdateWindowSurface(win);

    int running = 1;
    int mouse_down = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) running = 0;
                break;
            case SDL_MOUSEBUTTONDOWN:
                   if (e.button.button == SDL_BUTTON_LEFT) {
                    // Клик по квадрату?
                    if (point_in_rect(e.button.x, e.button.y, &square)) {
                        printf("Square clicked at (%d, %d)\\n", e.button.x, e.button.y);
                        fflush(stdout);
                    } else {
                        mouse_down = 1;
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) mouse_down = 0;
                break;
            case SDL_MOUSEMOTION:
                if (mouse_down) {
                    if (SDL_MUSTLOCK(surf)) SDL_LockSurface(surf);
                    int pitch_px = surf->pitch / 4;
                    put_pixel32((uint32_t*)surf->pixels, pitch_px, e.motion.x, e.motion.y, paint);
                    if (SDL_MUSTLOCK(surf)) SDL_UnlockSurface(surf);
                    // Перерисуем квадрат поверх, чтобы он оставался видимым
                    draw_filled_rect(surf, square.x, square.y, square.w, square.h, button);
                    SDL_UpdateWindowSurface(win);
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    e.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    surf = SDL_GetWindowSurface(win);
                    bg = SDL_MapRGB(surf->format, 0, 0, 0);
                    paint = SDL_MapRGB(surf->format, 255, 255, 255);
                    button = SDL_MapRGB(surf->format, 255, 0, 0);
                    center_square_rect(surf, SQUARE_SIDE, &square);
                    // Обновим экран: просто перерисуем квадрат поверх текущего содержимого
                    draw_filled_rect(surf, square.x, square.y, square.w, square.h, button);
                    // Сообщим новый размер области вывода
                    get_output_size(win, &ow, &oh);
                    printf("Размер области вывода изменён: %d x %d\n", ow, oh);
                    fflush(stdout);     
                    SDL_UpdateWindowSurface(win);
                }
                break;
            }
        }
        SDL_Delay(1);
    } 

    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
