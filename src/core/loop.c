// === file: src/core/loop.c ===
lh->len = w;
if (lh->len > 1) qsort(lh->arr, lh->len, sizeof(Hook), s_cmp_hook);
lh->need_sort = 0;
}


LoopHooks* loop_create(void) {
    struct LoopHooks* lh = (struct LoopHooks*)calloc(1, sizeof(*lh));
    return lh;
}


void loop_destroy(LoopHooks* lh) {
    if (!lh) return;
    for (size_t i=0;i<lh->len;i++) free(lh->arr[i].name);
    free(lh->arr);
    free(lh);
}


int loop_add_hook(LoopHooks* lh, LoopHookFn fn, void* user, const char* name, int priority) {
    if (!lh || !fn) return -1;
    if (lh->len == lh->cap) {
        size_t ncap = lh->cap ? lh->cap*2 : 8;
        Hook* n = (Hook*)realloc(lh->arr, ncap*sizeof(Hook));
        if (!n) return -1;
        lh->arr = n; lh->cap = ncap;
    }
    Hook h; memset(&h, 0, sizeof(h));
    h.fn = fn; h.user = user; h.priority = priority; h.active = 1; h.seq = ++lh->seq_ctr;
    h.name = name ? strdup(name) : NULL;
    lh->arr[lh->len++] = h;
    lh->need_sort = 1;
    if (!lh->in_run) s_compact_and_sort(lh);
    return 0;
}


void loop_remove_hook(LoopHooks* lh, LoopHookFn fn, void* user) {
    if (!lh || !fn) return;
    for (size_t i=0;i<lh->len;i++) {
        if (lh->arr[i].active && lh->arr[i].fn == fn && lh->arr[i].user == user) {
            lh->arr[i].active = 0;
            break;
        }
    }
    if (!lh->in_run) s_compact_and_sort(lh); else lh->need_sort = 1;
}


void loop_run_hooks(LoopHooks* lh, uint32_t now_ms) {
    if (!lh) return;
    lh->in_run = 1;
// локальная копия длины: новые хуки попадут в следующий кадр
    size_t n = lh->len;
    for (size_t i=0;i<n;i++) {
        Hook* h = &lh->arr[i];
        if (!h->active || !h->fn) continue;
        h->fn(h->user, now_ms);
    }
    lh->in_run = 0;
    if (lh->need_sort) s_compact_and_sort(lh);
}
