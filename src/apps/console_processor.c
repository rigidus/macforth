#include "console/processor.h"
#include "console/store.h"
#include "apps/widget_color.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct ConsoleProcessor {
    ConsoleStore* store;
};

ConsoleProcessor* con_processor_create(ConsoleStore* store){
    ConsoleProcessor* p = (ConsoleProcessor*)calloc(1, sizeof(ConsoleProcessor));
    if (!p) return NULL;
    p->store = store;
    return p;
}

void con_processor_destroy(ConsoleProcessor* p){
    if (!p) return;
    free(p);
}

static void reply(ConsoleProcessor* p, const char* s){
    con_store_append_line(p->store, s ? s : "");
}

static void trim_leading(const char** p){
    const char* s = *p;
    while (*s==' ' || *s=='\t') ++s;
    *p = s;
}

static int starts_with(const char* s, const char* kw){
    size_t n = strlen(kw);
    return strncmp(s, kw, n)==0 && (s[n]==0 || s[n]==' ' || s[n]=='\t');
}

void con_processor_on_command(ConsoleProcessor* p, const char* line){
    if (!p || !line) return;
    const char* s = line;
    trim_leading(&s);
    if (*s==0) return;

    if (starts_with(s, "help")){
        reply(p, "commands: help | echo <text> | time");
        return;
    }
    if (starts_with(s, "echo")){
        s += 4; trim_leading(&s);
        reply(p, s);
        return;
    }
    if (starts_with(s, "time")){
        char buf[64];
        unsigned ms = SDL_GetTicks();
        snprintf(buf, sizeof(buf), "time: %u ms since start", ms);
        reply(p, buf);
        return;
    }
    if (starts_with(s, "color")){
        /* команда создаёт пустой ColorSlider (равносильна drop, но без DnD) */
        ConsoleWidget* w = widget_color_create(128);
        if (w) con_store_append_widget(p->store, w);
        return;
    }
    reply(p, "unknown command. try 'help'");
}
