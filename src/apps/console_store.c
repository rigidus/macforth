#include "console/store.h"
#include "console/widget.h"
#include <stdlib.h>
#include <string.h>

struct SubEntry { ConsoleStoreListener cb; void* user; };

struct ConsoleStore {
    /* история (кольцевой буфер) */
    char *lines[CON_BUF_LINES];
    int   line_len[CON_BUF_LINES];
    ConsoleWidget* widgets[CON_BUF_LINES]; /* NULL — если это текстовая строка */
    /* стабильные ID элементов истории (строки и виджеты) */
    ConItemId ids[CON_BUF_LINES];
    ConItemId next_id;
    int   head;   /* индекс самой старой */
    int   count;  /* актуальное кол-во */

    /* редактируемая строка (общая для М1) */
    char  edit[CON_MAX_LINE];
    int   edit_len;

    /* подписчики */
    struct SubEntry subs[8];
    int subs_n;
};

static char* xstrdup_local(const char* s){
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void notify(ConsoleStore* st){
    for (int i=0;i<st->subs_n;i++){
        if (st->subs[i].cb) st->subs[i].cb(st->subs[i].user);
    }
}

ConsoleStore* con_store_create(void){
    ConsoleStore* st = (ConsoleStore*)calloc(1, sizeof(ConsoleStore));
    if (!st) return NULL;
    /* приветственные строки (как раньше) */
    const char* hello1 = "console ready. type here...";
    const char* hello2 = "press Enter to commit line";
    st->lines[0] = xstrdup_local(hello1); st->line_len[0] = (int)strlen(hello1);
    st->lines[1] = xstrdup_local(hello2); st->line_len[1] = (int)strlen(hello2);
    st->count = 2; st->head = 0;
    st->edit_len = 0; st->edit[0]=0;
    st->subs_n = 0;
    st->next_id = 1; /* 0 зарезервирован как INVALID */
    return st;
}

void con_store_destroy(ConsoleStore* st){
    if (!st) return;
    for (int i=0;i<CON_BUF_LINES;i++){
        if (st->lines[i]) free(st->lines[i]);
        if (st->widgets[i]) { con_widget_destroy(st->widgets[i]); st->widgets[i]=NULL; }
    }
    free(st);
}

void con_store_subscribe(ConsoleStore* st, ConsoleStoreListener cb, void* user){
    if (!st || !cb) return;
    if (st->subs_n < (int)(sizeof(st->subs)/sizeof(st->subs[0]))){
        st->subs[st->subs_n].cb   = cb;
        st->subs[st->subs_n].user = user;
        st->subs_n++;
    }
}

static void commit_line(ConsoleStore* st){
    int idx = (st->head + st->count) % CON_BUF_LINES;

    if (st->lines[idx]) { free(st->lines[idx]); st->lines[idx]=NULL; }
    if (st->widgets[idx]) { con_widget_destroy(st->widgets[idx]); st->widgets[idx]=NULL; }

    st->lines[idx] = (char*)malloc((size_t)st->edit_len + 1);
    if (st->lines[idx]){
        st->ids[idx] = st->next_id++;
        memcpy(st->lines[idx], st->edit, (size_t)st->edit_len);
        st->lines[idx][st->edit_len]=0;
        st->line_len[idx] = st->edit_len;
        if (st->count < CON_BUF_LINES) st->count++;
        else st->head = (st->head + 1) % CON_BUF_LINES;
    }
    st->edit_len=0; st->edit[0]=0;
}

static void append_line(ConsoleStore* st, const char* s){
    if (!s) return;
    int idx = (st->head + st->count) % CON_BUF_LINES;

    if (st->lines[idx]) { free(st->lines[idx]); st->lines[idx]=NULL; }
    if (st->widgets[idx]) { con_widget_destroy(st->widgets[idx]); st->widgets[idx]=NULL; }

    size_t n = strlen(s);
    st->lines[idx] = (char*)malloc(n + 1);
    if (st->lines[idx]){
        st->ids[idx] = st->next_id++;
        memcpy(st->lines[idx], s, n);
        st->lines[idx][n]=0;
        st->line_len[idx] = (int)n;
        if (st->count < CON_BUF_LINES) st->count++;
        else st->head = (st->head + 1) % CON_BUF_LINES;
    }
}

static ConItemId append_widget_int(ConsoleStore* st, ConsoleWidget* w){
    if (!w) return CON_ITEMID_INVALID;
    int idx = (st->head + st->count) % CON_BUF_LINES;
    if (st->lines[idx]) { free(st->lines[idx]); st->lines[idx]=NULL; }
    if (st->widgets[idx]) { con_widget_destroy(st->widgets[idx]); st->widgets[idx]=NULL; }
    st->widgets[idx] = w;
    st->line_len[idx] = 0;
    st->ids[idx] = st->next_id++;
    ConItemId id = st->ids[idx];
    if (st->count < CON_BUF_LINES) st->count++;
    else st->head = (st->head + 1) % CON_BUF_LINES;
    return id;
}

void con_store_put_text(ConsoleStore* st, const char* utf8){
    if (!st || !utf8) return;
    while (*utf8){
        unsigned char c = (unsigned char)*utf8++;
        if (st->edit_len < CON_MAX_LINE-1){
            st->edit[st->edit_len++] = (char)c;
            st->edit[st->edit_len] = 0;
        }
    }
    notify(st);
}

void con_store_append_line(ConsoleStore* st, const char* s){
    if (!st) return;
    append_line(st, s);
    notify(st);
}

ConItemId con_store_append_widget(ConsoleStore* st, ConsoleWidget* w){
    if (!st) return CON_ITEMID_INVALID;
    ConItemId id = append_widget_int(st, w);
    notify(st);
    return id;
}

void con_store_notify_changed(ConsoleStore* st){
    if (!st) return;
    notify(st);
}

void con_store_backspace(ConsoleStore* st){
    if (!st) return;
    if (st->edit_len > 0){
        st->edit[--st->edit_len] = 0;
        notify(st);
    }
}

void con_store_commit(ConsoleStore* st){
    if (!st) return;
    commit_line(st);
    notify(st);
}

int con_store_count(const ConsoleStore* st){
    return st ? st->count : 0;
}

const char* con_store_get_line(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return NULL;
    int phys = (st->head + index) % CON_BUF_LINES;
    return st->lines[phys];
}

int con_store_get_line_len(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return 0;
    int phys = (st->head + index) % CON_BUF_LINES;
    return st->line_len[phys];
}

int con_store_get_edit(const ConsoleStore* st, char* out, int cap){
    if (!st || !out || cap<=0) return 0;
    int n = st->edit_len;
    if (n > cap-1) n = cap-1;
    memcpy(out, st->edit, (size_t)n);
    out[n]=0;
    return n;
}

ConsoleWidget* con_store_get_widget(const ConsoleStore* st, int index){
    if (!st) return NULL;
    if (index<0 || index>=st->count) return NULL;
    int idx = (st->head + index) % CON_BUF_LINES;
    return st->widgets[idx];
}


ConItemId con_store_get_id(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return CON_ITEMID_INVALID;
    int phys = (st->head + index) % CON_BUF_LINES;
    return st->ids[phys];
}

int con_store_find_index_by_id(const ConsoleStore* st, ConItemId id){
    if (!st || id==CON_ITEMID_INVALID) return -1;
    for (int i=0;i<st->count;i++){
        int phys = (st->head + i) % CON_BUF_LINES;
        if (st->ids[phys] == id) return i;
    }
    return -1;
}

int con_store_widget_message(ConsoleStore* st, ConItemId id,
                             const char* tag, const void* data, size_t size){
    if (!st || id==CON_ITEMID_INVALID) return 0;
    int idx = con_store_find_index_by_id(st, id);
    if (idx < 0) return 0;
    int phys = (st->head + idx) % CON_BUF_LINES;
    ConsoleWidget* w = st->widgets[phys];
    if (!w || !w->on_message) return 0;
    int changed = w->on_message(w, tag, data, size);
    if (changed){
        notify(st);
    }
    return changed;
}
