#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct Surface Surface;

typedef struct {
    int x, y, w, h;
} Rect;

static inline Rect rect_make(int x,int y,int w,int h){ Rect r={x,y,w,h}; return r; }
static inline int  rect_is_empty(Rect r){ return r.w<=0 || r.h<=0; }
static inline Rect rect_intersect(Rect a, Rect b){
    int x0=a.x>b.x?a.x:b.x, y0=a.y>b.y?a.y:b.y;
    int x1=(a.x+a.w<b.x+b.w)?a.x+a.w:b.x+b.w;
    int y1=(a.y+a.h<b.y+b.h)?a.y+a.h:b.y+b.h;
    Rect r={x0,y0,x1-x0,y1-y0}; if(r.w<0)r.w=0; if(r.h<0)r.h=0; return r;
}

typedef struct {
    int user_id;
    int type; // 1=KEYDOWN,2=TEXT,3=MBUTTON,4=MMOTION,5=MWHEEL,6=WIN
    union {
        struct { int sym; int repeat; } key;
        struct { char text[32]; } text;
        struct { int x,y; int button; int state; int buttons; int dx,dy; int wheel_y; } mouse;
        struct { int event; int w,h; } win; // 1=RESIZE,2=EXPOSE
    };
} InputEvent;

struct Window;

typedef struct WindowVTable {
    void (*draw)(struct Window *w, const Rect *invalid);
    void (*on_event)(struct Window *w, const InputEvent *e, int lx, int ly);
    void (*tick)(struct Window *w, uint32_t now_ms);
    void (*on_focus)(struct Window *w, bool focused);
    void (*destroy)(struct Window *w);
} WindowVTable;

typedef struct Window {
    char   name[32];
    Rect   frame;
    int    zindex;
    bool   visible;
    bool   animating;
    uint32_t next_anim_ms;

    Surface *cache;     // ARGB32 per-window surface
    bool     invalid_all;

    struct { int dragging, dx, dy; } drag;

    const WindowVTable *vt;
    void *user; // per-window state
} Window;

void window_init(Window *w, const char *name, Rect frame, int z, const WindowVTable *vt);
void window_invalidate(Window *w, Rect area_screen);
