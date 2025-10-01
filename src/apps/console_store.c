#include "console/store.h"
#include "console/widget.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ==== CRDT позиция (Logoot/LSEQ-подобная) ==== */
static int pos_cmp(const ConPosId* a, const ConPosId* b){
    if (!a || !b) return 0;
    int da = a->depth, db = b->depth;
    int d = (da<db)?da:db;
    for (int i=0;i<d;i++){
        if (a->comp[i].digit != b->comp[i].digit)
            return (int)a->comp[i].digit - (int)b->comp[i].digit;
        if (a->comp[i].actor != b->comp[i].actor)
            return (a->comp[i].actor < b->comp[i].actor) ? -1 : 1;
    }
    /* префикс меньше более длинного */
    if (da != db) return (da < db) ? -1 : 1;
    return 0;
}
int con_pos_cmp(const ConPosId* a, const ConPosId* b){ return pos_cmp(a,b); }

static ConPosId pos_between_impl(const ConPosId* L, const ConPosId* R, uint32_t actor){
    const uint16_t BASE = 0xFFFF; /* 65535 */
    ConPosId out; memset(&out,0,sizeof(out));
    ConPosId left  = L? *L : (ConPosId){0};
    ConPosId right = R? *R : (ConPosId){ .depth=1, .comp={{BASE,0}} };
    int depth=0;
    for (;;){
        uint16_t ld = (depth<left.depth ) ? left.comp[depth].digit  : 0;
        uint16_t rd = (depth<right.depth) ? right.comp[depth].digit : BASE;
        if (rd - ld > 1){
            /* есть место */
            for (int i=0;i<depth;i++){
                if (i < left.depth){
                    out.comp[i] = left.comp[i];
                } else {
                    out.comp[i].digit = 0;
                    out.comp[i].actor = 0;
                }
            }
            out.comp[depth].digit = (uint16_t)(ld + (rd-ld)/2);
            out.comp[depth].actor = actor;
            out.depth = (uint8_t)(depth+1);
            return out;
        }
        depth++;
        /* если упёрлись — двигаем глубину */
        if (depth >= CON_POS_MAX_DEPTH){
            /* fallback: просто прижмёмся справа */
            out.depth = CON_POS_MAX_DEPTH;
            for (int i=0;i<CON_POS_MAX_DEPTH;i++){
                out.comp[i].digit = (i<left.depth)? left.comp[i].digit : 0;
                out.comp[i].actor = (i<left.depth)? left.comp[i].actor : 0;
            }
            out.comp[CON_POS_MAX_DEPTH-1].digit = (uint16_t)((out.comp[CON_POS_MAX_DEPTH-1].digit<BASE)? out.comp[CON_POS_MAX_DEPTH-1].digit+1 : BASE);
            out.comp[CON_POS_MAX_DEPTH-1].actor = actor;
            return out;
        }
    }
}


/* ===== Внутренние типы ===== */
struct SubEntry { ConsoleStoreListener cb; void* user; };

typedef struct ConEntry {
    ConEntryType  type;
    ConItemId     id;      /* стабильный ID */
    ConPosId      pos;     /* CRDT-позиция (лексикографическая) */
    int           user_id; /* источник (для окраски): -1 = системная/неизвестно */
    union {
        struct { char* s; int len; } text;
        ConsoleWidget* widget;
        struct { int dropped_count; } snap; /* CON_ENTRY_SNAPSHOT */
    } as;
} ConEntry;

struct ConsoleStore {
    ConEntry  entries[CON_BUF_LINES];
    int       head;
    int       count;

    struct SubEntry subs[8];
    int subs_n;

    ConItemId next_id;

    /* отсортированный порядок отображения: индексы в entries[] */
    int   order[CON_BUF_LINES];
    int   order_valid;

    /* --- очередь «точечных» изменений и флаг all для инкрементального редрава --- */
    ConItemId changes[CON_STORE_CHANGES_MAX];
    int       changes_n;
    int       changes_all; /* 1 — структура изменилась (вставки/компакция/пересортировка) */

    /* --- состояние промптов и индикаторы ввода (по user_id) --- */
    struct {
        int   len;
        int   edits;
        int   nonempty; /* 0/1 */
        char  buf[CON_MAX_LINE];
    } prompts[CON_MAX_USERS];
};

/* ===== Утилиты ===== */

static int phys_index(const ConsoleStore* st, int visible_index){
    if (!st || visible_index<0 || visible_index>=st->count) return -1;
    return st->order[visible_index];
}

static int has_id(const ConsoleStore* st, ConItemId id){
    for (int i=0;i<st->count;i++){ int p=(st->head+i)%CON_BUF_LINES; if (st->entries[p].id==id) return 1; }
    return 0;
}

static void free_entry(ConEntry* e){
    if (!e) return;
    if (e->type == CON_ENTRY_TEXT){
        free(e->as.text.s); e->as.text.s=NULL; e->as.text.len=0;
    } else if (e->type == CON_ENTRY_WIDGET){
        if (e->as.widget){ con_widget_destroy(e->as.widget); e->as.widget=NULL; }
    }
    e->type = 0; e->id = 0; e->pos = (ConPosId){0};
}


static void notify(ConsoleStore* st){
    st->order_valid = 0;
    for (int i=0;i<st->subs_n;i++){
        if (st->subs[i].cb) st->subs[i].cb(st->subs[i].user);
    }
}

/* ---- Параметры «свёртки хвоста» ----
   Оставляем не меньше CON_SNAPSHOT_KEEP_LAST последних записей,
   и сворачиваем пачкой не меньше CON_SNAPSHOT_MIN_DROP строк.
   Виджеты не удаляем. */
#ifndef CON_SNAPSHOT_KEEP_LAST
#  define CON_SNAPSHOT_KEEP_LAST 256
#endif
#ifndef CON_SNAPSHOT_MIN_DROP
#  define CON_SNAPSHOT_MIN_DROP  64
#endif

/* portable qsort (без qsort_r): компаратор читает глобальный указатель */
static const ConsoleStore* g_sort_store = NULL;
static int cmp_order_by_pos_id(const void* a, const void* b){
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    const ConEntry* ea = &g_sort_store->entries[ia];
    const ConEntry* eb = &g_sort_store->entries[ib];
    int c = pos_cmp(&ea->pos, &eb->pos);
    if (c<0) return -1;
    if (c>0) return  1;
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

/* ==== Трекинг изменений для инкрементальной перерисовки ==== */
static void changes_reset(ConsoleStore* st){ st->changes_n = 0; st->changes_all = 0; }
static void changes_mark_all(ConsoleStore* st){ st->changes_all = 1; }
static void changes_push(ConsoleStore* st, ConItemId id){
    if (st->changes_all) return; /* уже решено перерисовать всё */
    if (id==CON_ITEMID_INVALID) { st->changes_all = 1; return; }
    if (st->changes_n < CON_STORE_CHANGES_MAX){
        st->changes[st->changes_n++] = id;
    } else {
        /* переполнение очереди — переключаемся на полный redraw */
        st->changes_all = 1;
    }
}
int con_store_drain_changes(ConsoleStore* st, ConItemId* out_ids, int cap, int* out_all_flag){
    if (!st) return 0;
    int all = st->changes_all;
    int n = (st->changes_n < cap)? st->changes_n : cap;
    if (out_ids && n>0) memcpy(out_ids, st->changes, n*sizeof(ConItemId));
    changes_reset(st); if (out_all_flag) *out_all_flag = all; return n;
}

/* ===== Компактация хвоста =====
   Удаляем самые старые TEXT-строки (но не WIDGET), пока не останется минимум keep_last.
   Если удалили достаточно (>= min_drop), добавляем в конец строку-заглушку. */
static void maybe_compact_tail(ConsoleStore* st){
    if (!st) return;
    /* буфер ещё не под давлением — выходим */
    if (st->count < CON_BUF_LINES - 2) return;

    int drop = 0;
    /* удаляем с головы только TEXT, пока не достигнем keep_last */
    while (st->count > CON_SNAPSHOT_KEEP_LAST){
        ConEntry* head = &st->entries[st->head];
        /* если на голове виджет — не тянем дальше (сохраняем интерактив) */
        if (head->type == CON_ENTRY_WIDGET) break;
        /* TEXT — можно удалить */
        free_entry(head);
        st->head = (st->head + 1) % CON_BUF_LINES;
        st->count--;
        drop++;
    }
    if (drop >= CON_SNAPSHOT_MIN_DROP){
        /* Добавляем агрегатный SNAPSHOT-элемент вместо текстовой строки-заглушки */
        int idx = (st->head + st->count) % CON_BUF_LINES;
        free_entry(&st->entries[idx]);
        st->entries[idx].type = CON_ENTRY_SNAPSHOT;
        st->entries[idx].id   = st->next_id++;
        st->entries[idx].pos  = con_store_gen_between(st, con_store_last_id(st), CON_ITEMID_INVALID, 0);
        st->entries[idx].user_id = -1;
        st->entries[idx].as.snap.dropped_count = drop;
        if (st->count < CON_BUF_LINES) st->count++; else st->head = (st->head + 1) % CON_BUF_LINES;
        /* после компактации структура изменилась → полный redraw */
        changes_mark_all(st);
    }
}

/* ---- Поиск позиции элемента по ID (для gen_between) ---- */
static int find_phys_by_id(const ConsoleStore* st, ConItemId id){
    if (!st || id==CON_ITEMID_INVALID) return -1;
    for (int i=0;i<st->count;i++){
        int p=(st->head+i)%CON_BUF_LINES;
        if (st->entries[p].id==id) return p;
    }
    return -1;
}

ConPosId con_store_gen_between(const ConsoleStore* st, ConItemId left, ConItemId right, uint32_t actor){
    ConPosId L={0}, R={0}; ConPosId* pL=NULL; ConPosId* pR=NULL;
    if (left != CON_ITEMID_INVALID){
        int lp = find_phys_by_id(st, left);
        if (lp>=0){ L = st->entries[lp].pos; pL=&L; }
    }
    if (right != CON_ITEMID_INVALID){
        int rp = find_phys_by_id(st, right);
        if (rp>=0){ R = st->entries[rp].pos; pR=&R; }
    }
    return pos_between_impl(pL, pR, actor);
}

/* ===== Конструктор/деструктор ===== */
ConsoleStore* con_store_create(void){
    ConsoleStore* st = (ConsoleStore*)calloc(1, sizeof(ConsoleStore));
    if (!st) return NULL;
    st->subs_n = 0;
    /* Пустой стор без приветственных строк */
    st->next_id = 1;
    st->head = 0;
    st->count = 0;
    st->order_valid = 0;
    /* очередь изменений пуста */
    changes_reset(st);
    /* промпты по умолчанию пустые */
    for (int i=0;i<CON_MAX_USERS;i++){ st->prompts[i].len=0; st->prompts[i].buf[0]=0; st->prompts[i].edits=0; st->prompts[i].nonempty=0; }
    /* подписки/колбеки по умолчанию уже обнулены calloc'ом */
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

static void append_line_internal(ConsoleStore* st, const char* s){
    if (!s) return;
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_TEXT;
    st->entries[idx].id   = st->next_id++;
    st->entries[idx].pos  = con_store_gen_between(st, con_store_last_id(st), CON_ITEMID_INVALID, 0);
    st->entries[idx].user_id = -1;
    size_t n = strlen(s);
    st->entries[idx].as.text.s = (char*)malloc(n + 1);
    if (st->entries[idx].as.text.s){
        memcpy(st->entries[idx].as.text.s, s, n);
        st->entries[idx].as.text.s[n]=0;
        st->entries[idx].as.text.len = (int)n;
        if (st->count < CON_BUF_LINES) st->count++;
        else st->head = (st->head + 1) % CON_BUF_LINES;
    }
    /* изменение структуры (добавление) - проще пометить как all */
    changes_mark_all(st);
    /* компактацию не вызываем здесь, чтобы избежать рекурсии */
}

static ConItemId append_widget_internal(ConsoleStore* st, ConsoleWidget* w){
    if (!w) return CON_ITEMID_INVALID;
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_WIDGET;
    st->entries[idx].id   = st->next_id++;
    st->entries[idx].pos  = con_store_gen_between(st, con_store_last_id(st), CON_ITEMID_INVALID, 0);
    st->entries[idx].user_id = -1;
    st->entries[idx].as.widget = w;
    ConItemId id = st->entries[idx].id;
    if (st->count < CON_BUF_LINES) st->count++;
    else st->head = (st->head + 1) % CON_BUF_LINES;
    /* компактацию не вызываем здесь, чтобы избежать рекурсии */
    return id;
}

void con_store_append_line(ConsoleStore* st, const char* s){
    if (!st) return;
    append_line_internal(st, s);
    maybe_compact_tail(st);
    notify(st);
    changes_mark_all(st);
}

ConItemId con_store_append_widget(ConsoleStore* st, ConsoleWidget* w){
    if (!st) return CON_ITEMID_INVALID;
    ConItemId id = append_widget_internal(st, w);
    notify(st);
    maybe_compact_tail(st);
    changes_mark_all(st);
    return id;
}

void con_store_notify_changed(ConsoleStore* st){
    if (!st) return;
    /* компактация по явному notify не нужна — вызовы идут при внутренних изменениях */
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

int con_store_get_user(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return -1;
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return -1;
    return st->entries[phys].user_id;
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
        /* точечное изменение конкретного элемента */
        changes_push(st, id);
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


ConItemId con_store_insert_text_between(ConsoleStore* st, ConItemId left, ConItemId right, const char* s){
    if (!st || !s) return CON_ITEMID_INVALID;
    ConPosId pos = con_store_gen_between(st, left, right, 0);
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
        changes_mark_all(st); /* вставка меняет порядки — безопасно перерисовать всё */
        notify(st);
        return st->entries[idx].id;
    }
    return CON_ITEMID_INVALID;
}

ConItemId con_store_insert_text_at(ConsoleStore* st, ConItemId forced_id, const ConPosId* pos, const char* s, int user_id){
    if (!st || !pos || !s) return CON_ITEMID_INVALID;
    if (forced_id && has_id(st, forced_id)) return forced_id; /* идемпотентность */
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_TEXT;
    st->entries[idx].id   = forced_id ? forced_id : st->next_id++;
    st->entries[idx].pos  = *pos;
    st->entries[idx].user_id = (user_id>=0)? user_id : -1;
    size_t n = strlen(s);
    st->entries[idx].as.text.s = (char*)malloc(n+1);
    if (st->entries[idx].as.text.s){
        memcpy(st->entries[idx].as.text.s, s, n);
        st->entries[idx].as.text.s[n]=0;
        st->entries[idx].as.text.len = (int)n;
        if (st->count < CON_BUF_LINES) st->count++; else st->head = (st->head + 1) % CON_BUF_LINES;
        changes_mark_all(st); /* структура менялась */
        notify(st);
        maybe_compact_tail(st);
        return st->entries[idx].id;
    }
    return CON_ITEMID_INVALID;
}

ConItemId con_store_insert_widget_at(ConsoleStore* st, ConItemId forced_id, const ConPosId* pos, ConsoleWidget* w, int user_id){
    if (!st || !pos || !w) return CON_ITEMID_INVALID;
    if (forced_id && has_id(st, forced_id)) { con_widget_destroy(w); return forced_id; }
    int idx = (st->head + st->count) % CON_BUF_LINES;
    free_entry(&st->entries[idx]);
    st->entries[idx].type = CON_ENTRY_WIDGET;
    st->entries[idx].id   = forced_id ? forced_id : st->next_id++;
    st->entries[idx].pos  = *pos;
    st->entries[idx].user_id = (user_id>=0)? user_id : -1;
    st->entries[idx].as.widget = w;
    if (st->count < CON_BUF_LINES) st->count++; else st->head = (st->head + 1) % CON_BUF_LINES;
    changes_mark_all(st); /* структура менялась */
    notify(st);
    maybe_compact_tail(st);
    return st->entries[idx].id;
}

ConItemId con_store_last_id(const ConsoleStore* st){
    if (!st || st->count<=0) return CON_ITEMID_INVALID;
    /* найдём самый правый по order */
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = st->order[st->count-1];
    return st->entries[phys].id;
}

/* ===== Снапшоты ===== */
int con_store_get_snapshot_dropped(const ConsoleStore* st, int index){
    if (!st || index<0 || index>=st->count) return -1;
    if (!st->order_valid) rebuild_order((ConsoleStore*)st);
    int phys = phys_index(st, index);
    if (phys<0) return -1;
    const ConEntry* e = &st->entries[phys];
    if (e->type != CON_ENTRY_SNAPSHOT) return -1;
    return e->as.snap.dropped_count;
}

/* ===== Промпты ===== */
static inline int valid_uid(int uid){ return uid>=0 && uid<CON_MAX_USERS; }

void con_store_prompt_insert(ConsoleStore* st, int user_id, const char* utf8, int bump){
    if (!st || !valid_uid(user_id) || !utf8) return;
    int *len = &st->prompts[user_id].len;
    char* buf =  st->prompts[user_id].buf;
    while (*utf8 && *len < CON_MAX_LINE-1){
        buf[(*len)++] = *utf8++;
    }
    buf[*len]=0;
    st->prompts[user_id].nonempty = (*len>0)?1:0;
    if (bump) st->prompts[user_id].edits++;
    notify(st);
}

void con_store_prompt_backspace(ConsoleStore* st, int user_id, int bump){
    if (!st || !valid_uid(user_id)) return;
    int *len = &st->prompts[user_id].len;
    if (*len>0){ st->prompts[user_id].buf[--(*len)] = 0; }
    st->prompts[user_id].nonempty = (*len>0)?1:0;
    if (bump) st->prompts[user_id].edits++;
    notify(st);
}

int con_store_prompt_take(ConsoleStore* st, int user_id, char* out, int cap){
    if (!st || !valid_uid(user_id) || !out || cap<=0) return 0;
    int n = st->prompts[user_id].len;
    if (n > cap-1) n = cap-1;
    if (n>0) memcpy(out, st->prompts[user_id].buf, n);
    out[n]=0;
    st->prompts[user_id].len = 0;
    st->prompts[user_id].buf[0]=0;
    st->prompts[user_id].nonempty = 0;
    notify(st);
    return n;
}

int con_store_prompt_peek(const ConsoleStore* st, int user_id, char* out, int cap){
    if (!st || !valid_uid(user_id) || !out || cap<=0) return 0;
    int n = st->prompts[user_id].len;
    if (n > cap-1) n = cap-1;
    if (n>0) memcpy(out, st->prompts[user_id].buf, n);
    out[n]=0;
    return n;
}

int con_store_prompt_len(const ConsoleStore* st, int user_id){
    if (!st || !valid_uid(user_id)) return 0;
    return st->prompts[user_id].len;
}

void con_store_prompt_get_meta(const ConsoleStore* st, int user_id, int* out_nonempty, int* out_edits){
    if (!st || !valid_uid(user_id)) return;
    if (out_nonempty) *out_nonempty = st->prompts[user_id].nonempty;
    if (out_edits)    *out_edits    = st->prompts[user_id].edits;
}

void con_store_prompt_apply_meta(ConsoleStore* st, int user_id, int edits_inc, int nonempty_flag){
    if (!st || !valid_uid(user_id)) return;
    if (edits_inc>0) st->prompts[user_id].edits += edits_inc;
    st->prompts[user_id].nonempty = nonempty_flag ? 1 : 0;
    /* содержимое buf не трогаем — оно локальное */
    notify(st);
}
