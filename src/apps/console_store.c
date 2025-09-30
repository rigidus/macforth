#include "console/store.h"
#include "console/widget.h"
#include <stdlib.h>
#include <string.h>

/* ===== Внутренние типы ===== */
struct SubEntry { ConsoleStoreListener cb; void* user; };

typedef struct ConEntry {
    ConEntryType  type;
    ConItemId     id;      /* стабильный ID */
    uint64_t      pos;     /* позиционный ключ (для сортировки) */
    union {
        struct { char* s; int len; } text;
        ConsoleWidget* widget;
    } as;
} ConEntry;

struct ConsoleStore {
    ConEntry  entries[CON_BUF_LINES];
    int       head;
    int       count;

    char  edit[CON_MAX_LINE];
    int   edit_len;

    struct SubEntry subs[8];
    int subs_n;

    ConItemId next_id;
    uint64_t  last_pos;

    /* отсортированный порядок отображения: индексы в entries[] */
    int   order[CON_BUF_LINES];
    int   order_valid;
};

/* ===== Утилиты ===== */
static int phys_index(const ConsoleStore* st, int visible_index){
    if (!st || visible_index<0 || visible_index>=st->count) return -1;
    return st->order[visible_index];
}

static void free_entry(ConEntry* e){
    if (!e) return;
    if (e->type == CON_ENTRY_TEXT){
        free(e->as.text.s); e->as.text.s=NULL; e->as.text.len=0;
    } else if (e->type == CON_ENTRY_WIDGET){
        if (e->as.widget){ con_widget_destroy(e->as.widget); e->as.widget=NULL; }
    }
    e->type = 0; e->id = 0; e->pos = 0;
}

static uint64_t next_pos_after(const ConsoleStore* st){
    uint64_t base = st ? st->last_pos : 0;
    const uint64_t STEP = (uint64_t)1<<32;
    uint64_t np = base + STEP;
    if (np == 0) np = STEP;
    return np;
}

static uint64_t pos_between(uint64_t left, uint64_t right){
    if (left == 0 && right == 0) return ((uint64_t)1<<32);
    if (left == 0) return right / 2;
    if (right == 0) return left + (((uint64_t)1<<32));
    if (left + 1 >= right) return left + 1;
    return left + (right - left)/2;
}

static void notify(ConsoleStore* st){
    st->order_valid = 0;
    for (int i=0;i<st->subs_n;i++){
        if (st->subs[i].cb) st->subs[i].cb(st->subs[i].user);
    }
}

/* portable qsort (без qsort_r): компаратор читает глобальный указатель */
static const ConsoleStore* g_sort_store = NULL;
static int cmp_order_by_pos_id(const void* a, const void* b){
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    const ConEntry* ea = &g_sort_store->entries[ia];
    const ConEntry* eb = &g_sort_store->entries[ib];
    if (ea->pos < eb->pos) return -1;
    if (ea->pos > eb->pos) return  1;
    if (ea->id  < eb->id)  return -1;
    if (ea->id  > eb->id)  return  1;
    return 0;
}
static void rebuild_order(ConsoleStore* st){
    if (!st) return;
    int n = st->count;
    for (int i=0;i<n;i++){
        st->order[i] = (st->head + i) % CON_BUF_LINES;
    }
    g_sort_store = st;
    qsort(st->order, n, sizeof(int), cmp_order_by_pos_id);
    g_sort_store = NULL;
    st->order_valid = 1;
}

/* ===== Конструктор/деструктор ===== */
ConsoleStore* con_store_create(void){
    ConsoleStore* st = (ConsoleStore*)calloc(1, sizeof(ConsoleStore));
    if (!st) return NULL;
    st->head = 0; st->count = 0;
    st->edit_len = 0; st->edit[0]=0;
    st->subs_n = 0;
    st->next_id = 1;
    st->last_pos = 0;
    st->order_valid = 0;

    /* приветствие как обычные TEXT-entry (без notify — подписчиков ещё нет) */
    {
        int idx = (st->head + st->count) % CON_BUF_LINES;
        free_entry(&st->entries[idx]);
        st->entries[idx].type = CON_ENTRY_TEXT;
        st->entries[idx].id   = st->next_id++;
        st->entries[idx].pos  = st->last_pos = next_pos_after(st);
        const char* hello1 = "console ready. type here...";
        size_t n = strlen(hello1);
        st->entries[idx].as.text.s = (char*)malloc(n+1);
        if (st->entries[idx].as.text.s){
            memcpy(st->entries[idx].as.text.s, hello1, n);
            st->entries[idx].as.text.s[n]=0;
            st->entries[idx].as.text.len = (int)n;
            if (st->count < CON_BUF_LINES) st->count++;
            else st->head = (st->head + 1) % CON_BUF_LINES;
        }
    }
    {
        int idx = (st->head + st->count) % CON_BUF_LINES;
        free_entry(&st->entries[idx]);
        st->entries[idx].type = CON_ENTRY_TEXT;
        st->entries[idx].id   = st->next_id++;
        st->entries[idx].pos  = st->last_pos = next_pos_after(st);
        const char* hello2 = "press Enter to commit line";
        size_t n = strlen(hello2);
        st->entries[idx].as.text.s = (char*)malloc(n+1);
        if (st->entries[idx].as.text.s){
            memcpy(st->entries[idx].as.text.s, hello2, n);
            st->entries[idx].as.text.s[n]=0;
            st->entries[idx].as.text.len = (int)n;
            if (st->count < CON_BUF_LINES) st->count++;
            else st->head = (st->head + 1) % CON_BUF_LINES;
        }
    }
    return st;
}

void con_store_destroy(ConsoleStore* st){
    if (!st) return;
    for (int i=0;i<CON_BUF_LINES;i++) free_entry(&st->entries[i]);
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

/* ===== Внутренние «append/commit» без notify ===== */
static void commit_line(ConsoleStore* st){
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_TEXT;
    st->entries[idx].id   = st->next_id++;
    st->entries[idx].pos  = st->last_pos = next_pos_after(st);
    st->entries[idx].as.text.s = (char*)malloc((size_t)st->edit_len + 1);
    if (st->entries[idx].as.text.s){
        memcpy(st->entries[idx].as.text.s, st->edit, (size_t)st->edit_len);
        st->entries[idx].as.text.s[st->edit_len]=0;
        st->entries[idx].as.text.len = st->edit_len;
        if (st->count < CON_BUF_LINES) st->count++;
        else st->head = (st->head + 1) % CON_BUF_LINES;
    }
    st->edit_len=0; st->edit[0]=0;
}

static void append_line_internal(ConsoleStore* st, const char* s){
    if (!s) return;
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_TEXT;
    st->entries[idx].id   = st->next_id++;
    st->entries[idx].pos  = st->last_pos = next_pos_after(st);
    size_t n = strlen(s);
    st->entries[idx].as.text.s = (char*)malloc(n + 1);
    if (st->entries[idx].as.text.s){
        memcpy(st->entries[idx].as.text.s, s, n);
        st->entries[idx].as.text.s[n]=0;
        st->entries[idx].as.text.len = (int)n;
        if (st->count < CON_BUF_LINES) st->count++;
        else st->head = (st->head + 1) % CON_BUF_LINES;
    }
}

static ConItemId append_widget_internal(ConsoleStore* st, ConsoleWidget* w){
    if (!w) return CON_ITEMID_INVALID;
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_WIDGET;
    st->entries[idx].id   = st->next_id++;
    st->entries[idx].pos  = st->last_pos = next_pos_after(st);
    st->entries[idx].as.widget = w;
    ConItemId id = st->entries[idx].id;
    if (st->count < CON_BUF_LINES) st->count++;
    else st->head = (st->head + 1) % CON_BUF_LINES;
    return id;
}

/* ===== Публичный API ===== */
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
    append_line_internal(st, s);
    notify(st);
}

ConItemId con_store_append_widget(ConsoleStore* st, ConsoleWidget* w){
    if (!st) return CON_ITEMID_INVALID;
    ConItemId id = append_widget_internal(st, w);
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
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return NULL;
    const ConEntry* e = &st->entries[phys];
    if (e->type != CON_ENTRY_TEXT) return NULL;
    return e->as.text.s;
}

int con_store_get_line_len(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return 0;
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return 0;
    const ConEntry* e = &st->entries[phys];
    if (e->type != CON_ENTRY_TEXT) return 0;
    return e->as.text.len;
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
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return NULL;
    const ConEntry* e = &st->entries[phys];
    if (e->type != CON_ENTRY_WIDGET) return NULL;
    return e->as.widget;
}

ConItemId con_store_get_id(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return CON_ITEMID_INVALID;
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return CON_ITEMID_INVALID;
    return st->entries[phys].id;
}

int con_store_find_index_by_id(const ConsoleStore* st, ConItemId id){
    if (!st || id==CON_ITEMID_INVALID) return -1;
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int n = st->count;
    int phys_of_id = -1;
    for (int i=0;i<n;i++){
        int phys = (st->head + i) % CON_BUF_LINES;
        if (st->entries[phys].id == id){ phys_of_id = phys; break; }
    }
    if (phys_of_id<0) return -1;
    for (int i=0;i<n;i++){
        if (st->order[i] == phys_of_id) return i;
    }
    return -1;
}

int con_store_widget_message(ConsoleStore* st, ConItemId id,
                             const char* tag, const void* data, size_t size){
    if (!st || id==CON_ITEMID_INVALID) return 0;
    int phys = -1;
    for (int i=0;i<st->count;i++){
        int p = (st->head + i) % CON_BUF_LINES;
        if (st->entries[p].id == id){ phys = p; break; }
    }
    if (phys < 0) return 0;
    ConsoleWidget* w = (st->entries[phys].type==CON_ENTRY_WIDGET)? st->entries[phys].as.widget : NULL;
    if (!w || !w->on_message) return 0;
    int changed = w->on_message(w, tag, data, size);
    if (changed){
        notify(st);
    }
    return changed;
}

ConEntryType con_store_get_type(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return 0;
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return 0;
    return st->entries[phys].type;
}

uint64_t con_store_get_pos(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return 0;
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return 0;
    return st->entries[phys].pos;
}

ConItemId con_store_insert_text_between(ConsoleStore* st, ConItemId left, ConItemId right, const char* s){
    if (!st || !s) return CON_ITEMID_INVALID;
    uint64_t lpos = 0, rpos = 0;
    if (left != CON_ITEMID_INVALID){
        for (int i=0;i<st->count;i++){
            int p = (st->head + i) % CON_BUF_LINES;
            if (st->entries[p].id == left){ lpos = st->entries[p].pos; break; }
        }
    }
    if (right != CON_ITEMID_INVALID){
        for (int i=0;i<st->count;i++){
            int p = (st->head + i) % CON_BUF_LINES;
            if (st->entries[p].id == right){ rpos = st->entries[p].pos; break; }
        }
    }
    uint64_t pos = pos_between(lpos, rpos);
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_TEXT;
    st->entries[idx].id   = st->next_id++;
    st->entries[idx].pos  = pos;
    size_t n = strlen(s);
    st->entries[idx].as.text.s = (char*)malloc(n+1);
    if (st->entries[idx].as.text.s){
        memcpy(st->entries[idx].as.text.s, s, n);
        st->entries[idx].as.text.s[n]=0;
        st->entries[idx].as.text.len = (int)n;
        if (st->count < CON_BUF_LINES) st->count++;
        else st->head = (st->head + 1) % CON_BUF_LINES;
        notify(st);
        return st->entries[idx].id;
    }
    return CON_ITEMID_INVALID;
}
