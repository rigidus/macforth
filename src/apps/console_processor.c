#include "console/processor.h"
#include "console/store.h"
#include "console/sink.h"
#include "apps/widget_color.h"
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

struct ConsoleProcessor {
    ConsoleStore* store;
    ConsoleSink*  sink;  /* публикация ответов/виджетов через Sink */
};

/* вспомогательное — распечатать список виджетов с их ID (последние N) */
static void cmd_widgets(ConsoleProcessor* p){
    int n = con_store_count(p->store);
    int shown = 0;
    for (int i=(n>16? n-16:0); i<n; ++i){
        ConsoleWidget* w = con_store_get_widget(p->store, i);
        if (!w) continue;
        ConItemId id = con_store_get_id(p->store, i);
        char line[128];
        char buf[64]; buf[0]=0;
        const char* text = w->as_text? w->as_text(w, buf, (int)sizeof(buf)) : "[widget]";
        SDL_snprintf(line, sizeof(line), "widget id=%" PRIu64 " %s", (uint64_t)id, text?text:"");
        if (p->sink) con_sink_append_line(p->sink, -1, line);
        shown++;
    }
    if (shown==0 && p->sink) con_sink_append_line(p->sink, -1, "(no widgets in recent history)");
}

static void reply(ConsoleProcessor* p, const char* s){
    if (p->sink) con_sink_append_line(p->sink, -1, s ? s : "");
}

/* color set <id> <0..255> */
static void cmd_color_set(ConsoleProcessor* p, const char* s){
    ConItemId id = 0; int val = -1;
    if (sscanf(s, "%" SCNu64 " %d", (uint64_t*)&id, &val) != 2){ reply(p, "usage: color set <id> <0..255>"); return; }
    if (val<0 || val>255){ reply(p, "value must be 0..255"); return; }
    if (p->sink) con_sink_widget_message(p->sink, -1, id, "set", &val, sizeof(val));
}

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

void con_processor_set_sink(ConsoleProcessor* p, ConsoleSink* s){ if (p) p->sink = s; }

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
        reply(p, "commands: help | echo <text> | time | color | widgets | color set <id> <0..255>");
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
        /* всегда добавляем виджет в конец ленты */
        if (p->sink) con_sink_insert_widget_color(p->sink, -1, 128);
        return;
    }
    if (starts_with(s, "widgets")){
        cmd_widgets(p);
        return;
    }
    if (starts_with(s, "color set")){
        s += strlen("color set");
        trim_leading(&s);
        cmd_color_set(p, s);
        return;
    }
    reply(p, "unknown command. try 'help'");
}
