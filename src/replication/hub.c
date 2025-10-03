#include "replication/hub.h"
#include "common/conop.h"
#include "replication/repl_policy_default.h"
#include "replication/type_registry.h"
#include <stdlib.h>
#include <string.h>

#ifndef HUB_MAX_BACKENDS
#  define HUB_MAX_BACKENDS 8
#endif
#ifndef HUB_MAX_TOPICS
#  define HUB_MAX_TOPICS 64
#endif

typedef struct {
    TopicId               topic;
    int                   have;   /* 0/1 */
    int                   bidx;   /* выбранный backend index */
    ReplicatorConfirmCb   cb;     /* внешний колбэк */
    void*                 user;
    /* Этап 9: мягкий свитч — блокировка и буфер */
    int                   blocked;
    struct BufOp {
        ConOp   op;      /* полная копия скаляров */
        /* NB: tag/data/init скопированы, чтобы безопасно буферизовать */
        char*   tag;     /* если был */
        void*   data;    size_t dlen;
        void*   init;    size_t ilen;
    } *q;
    int qn, qcap;
    int required_caps;   // битмаска требований к бэкенду
    int forced_bidx;     // -1 если нет пина
} TopicRoute;

typedef struct {
    ReplBackendRef  refs[HUB_MAX_BACKENDS];
    int             n;
    int             adopt;
    int             required_caps;
    /* кэш возможностей/здоровья */
    int             caps[HUB_MAX_BACKENDS];
    /* маршруты на темы */
    TopicRoute      routes[HUB_MAX_TOPICS];
    int             rn;
} HubImpl;

static TopicId topic_from_op(const ConOp* op){
    /* Предпочитаем явно заданный topic; иначе по умолчанию console/console_id */
    TopicId t = {0,0};
    if (op){
        t = op->topic;
        if (t.type_id == 0 && t.inst_id == 0){
            t.type_id = 1u;
            t.inst_id = op->console_id;
        }
    }
    return t;
}

/* сравнение TopicId */
static int topic_eq(TopicId a, TopicId b){ return a.type_id==b.type_id && a.inst_id==b.inst_id; }

/* Внутренний колбэк: пробрасываем наружу по найденному route. */
static void hub_on_confirm(void* user, const ConOp* op){
    HubImpl* h = (HubImpl*)user; if (!h || !op) return;
    for (int i=0;i<h->rn;i++){
        if (h->routes[i].have && topic_eq(h->routes[i].topic, topic_from_op(op))) {
            if (h->routes[i].cb) h->routes[i].cb(h->routes[i].user, op);
            return;
        }
    }
}

/* === Буферизация/клонирование ConOp для мягкого свитча === */
static void bufop_free(struct BufOp* b){
    if (!b) return;
    free(b->tag);    b->tag = NULL;
    free(b->data);   b->data = NULL; b->dlen = 0;
    free(b->init);   b->init = NULL; b->ilen = 0;
    memset(&b->op, 0, sizeof(b->op));
}

static int route_queue_push(TopicRoute* r, const ConOp* op){
    char* tcopy=NULL; void* dcopy=NULL; void* icopy=NULL;
    size_t dlen=0, ilen=0;
    /* заранее клонируем */
    if (op->tag){
        size_t L = strlen(op->tag);
        tcopy = (char*)malloc(L+1); if (!tcopy) goto fail;
        memcpy(tcopy, op->tag, L+1);
    }
    if (op->data && op->size){
        dcopy = malloc(op->size); if(!dcopy) goto fail;
        memcpy(dcopy, op->data, op->size); dlen = op->size;
    }
    if (op->init_blob && op->init_size){
        icopy = malloc(op->init_size); if(!icopy) goto fail;
        memcpy(icopy, op->init_blob, op->init_size); ilen = op->init_size;
    }
    /* ensure capacity */
    if (r->qn == r->qcap){
        int ncap = r->qcap ? r->qcap*2 : 8;
        void* nb = realloc(r->q, (size_t)ncap * sizeof(*r->q));
        if (!nb) goto fail;
        r->q = (struct BufOp*)nb; r->qcap = ncap;
    }
    struct BufOp* dst = &r->q[r->qn];
    memset(dst, 0, sizeof(*dst));
    dst->op = *op;
    dst->tag = tcopy; dst->data = dcopy; dst->dlen = dlen; dst->init = icopy; dst->ilen = ilen;
    if (tcopy) dst->op.tag = tcopy;
    if (dcopy){ dst->op.data = dcopy; dst->op.size = dlen; }
    if (icopy){ dst->op.init_blob = icopy; dst->op.init_size = ilen; }
    r->qn++;
    return 1;
fail:
    free(tcopy); free(dcopy); free(icopy);
    return 0;
}


static int hub_choose(HubImpl* h, TopicId topic){
    /* per-topic настройки по умолчанию: от хаба */
    int required = h->required_caps;
    int forced   = -1;
    /* если у топика уже есть маршрут — используем его политику */
    for (int i=0;i<h->rn;i++){
        if (h->routes[i].have && topic_eq(h->routes[i].topic, topic)){
            required = h->routes[i].required_caps;
            forced   = h->routes[i].forced_bidx;
            break;
        }
    }
    PolicyCandidate cand[HUB_MAX_BACKENDS];
    for (int i=0;i<h->n;i++){
        cand[i].r = h->refs[i].r;
        cand[i].priority = h->refs[i].priority;
        int caps = 0, hl = -1;
        if (cand[i].r && cand[i].r->v && cand[i].r->v->capabilities) caps = cand[i].r->v->capabilities(cand[i].r);
        if (cand[i].r && cand[i].r->v && cand[i].r->v->health)       hl   = cand[i].r->v->health(cand[i].r);
        cand[i].caps = caps; cand[i].health = hl;
        h->caps[i] = caps;
    }
    /* Если есть принудительный выбор — попытаться его применить, проверив здоровье и cap-ы. */
    if (forced >= 0 && forced < h->n){
        Replicator* rr = h->refs[forced].r;
        int caps = 0, hl = -1;
        if (rr && rr->v && rr->v->capabilities) caps = rr->v->capabilities(rr);
        if (rr && rr->v && rr->v->health)       hl   = rr->v->health(rr);
        if (rr && hl==0 && ((caps & required) == required)){
            return forced;
        }
        /* иначе — падаем на политику выбора */
    }
    /* обычный выбор по required капам именно этого топика */
    int idx = repl_policy_default_choose(cand, h->n, required);
    if (idx < 0){
        /* fallback: если вообще никого — ничего не делаем */
        return -1;
    }
    return idx;
}

/* ===== VTable ===== */
static void hub_destroy(Replicator* rr){
    if (!rr) return;
    HubImpl* h = (HubImpl*)rr->impl;
    if (h){
        /* очистить буферы в маршрутах */
        for (int i=0;i<h->rn;i++){
            for (int j=0;j<h->routes[i].qn;j++) bufop_free(&h->routes[i].q[j]);
            free(h->routes[i].q);
        }
        if (h->adopt){
            for (int i=0;i<h->n;i++){
                if (h->refs[i].r) replicator_destroy(h->refs[i].r);
            }
        }
        free(h);
    }
    free(rr);
}

static void hub_publish(Replicator* rr, const ConOp* op){
    if (!rr || !op) return;
    HubImpl* h = (HubImpl*)rr->impl;
    TopicId t = topic_from_op(op);
    /* найдём/создадим маршрут */
    int ri = -1;
    for (int i=0;i<h->rn;i++) if (h->routes[i].have && topic_eq(h->routes[i].topic, t)){ ri=i; break; }
    if (ri < 0){
        if (h->rn < HUB_MAX_TOPICS){
            ri = h->rn++;
            h->routes[ri].have=1; h->routes[ri].topic = t; h->routes[ri].bidx=-1;
            h->routes[ri].cb=NULL; h->routes[ri].user=NULL;
            h->routes[ri].blocked=0; h->routes[ri].q=NULL; h->routes[ri].qn=0; h->routes[ri].qcap=0;
            h->routes[ri].required_caps = h->required_caps;
            h->routes[ri].forced_bidx   = -1;
        }
    }
    /* Если идёт мягкий свитч — буферизуем */
    if (ri>=0 && h->routes[ri].blocked){
        (void)route_queue_push(&h->routes[ri], op);
        return;
    }
    int want = hub_choose(h, t);
    if (ri>=0 && want>=0 && want != h->routes[ri].bidx){
        /* снять listener со старого бэкенда (если был) */
        int old = h->routes[ri].bidx;
        if (h->routes[ri].cb && old>=0 &&
            h->refs[old].r && h->refs[old].r->v && h->refs[old].r->v->unset_listener)
        {
            h->refs[old].r->v->unset_listener(h->refs[old].r, t);
        }
        /* переподпишемся на новый бэкенд, если есть внешний listener */
        if (h->routes[ri].cb && h->refs[want].r && h->refs[want].r->v && h->refs[want].r->v->set_listener){
            h->refs[want].r->v->set_listener(h->refs[want].r, t, hub_on_confirm, h);
        }
        h->routes[ri].bidx = want;
    }
    int use = (ri>=0) ? h->routes[ri].bidx : want;
    if (use>=0 && h->refs[use].r && h->refs[use].r->v && h->refs[use].r->v->publish){
        h->refs[use].r->v->publish(h->refs[use].r, op);
    }
}

static void hub_set_listener(Replicator* rr, TopicId topic, ReplicatorConfirmCb cb, void* user){
    if (!rr) return;
    HubImpl* h = (HubImpl*)rr->impl;
    /* найти/создать маршрут */
    int ri = -1;
    for (int i=0;i<h->rn;i++) if (h->routes[i].have && topic_eq(h->routes[i].topic, topic)){ ri=i; break; }
    if (ri < 0){
        if (h->rn < HUB_MAX_TOPICS){
            ri = h->rn++; h->routes[ri].have=1; h->routes[ri].topic = topic; h->routes[ri].bidx=-1;
            h->routes[ri].cb=NULL; h->routes[ri].user=NULL;
            h->routes[ri].blocked=0; h->routes[ri].q=NULL; h->routes[ri].qn=0; h->routes[ri].qcap=0;
            h->routes[ri].required_caps = h->required_caps;
            h->routes[ri].forced_bidx   = -1;
        }
    }
    if (ri < 0) return;
    /* запомним прежний бэкенд маршрута, чтобы корректно снять listener */
    int old = h->routes[ri].bidx;
    h->routes[ri].cb = cb;
    h->routes[ri].user = user;
    /* выбрать лучший и подписаться */
    int idx = hub_choose(h, topic);
    /* если меняем бэкенд — снять listener со старого */
    if (old>=0 && idx!=old &&
        h->refs[old].r && h->refs[old].r->v && h->refs[old].r->v->unset_listener)
    {
        h->refs[old].r->v->unset_listener(h->refs[old].r, topic);
    }
    h->routes[ri].bidx = idx;
    if (idx>=0 && h->refs[idx].r && h->refs[idx].r->v && h->refs[idx].r->v->set_listener){
        h->refs[idx].r->v->set_listener(h->refs[idx].r, topic, hub_on_confirm, h);
    }
}

static void hub_unset_listener(Replicator* rr, TopicId topic){
    if (!rr) return;
    HubImpl* h = (HubImpl*)rr->impl;
    for (int i=0;i<h->rn;i++){
        TopicRoute* R = &h->routes[i];
        if (R->have && topic_eq(R->topic, topic)){
            /* снять listener на активном бэкенде, если поддерживает */
            int b = R->bidx;
            if (b>=0 && h->refs[b].r && h->refs[b].r->v && h->refs[b].r->v->unset_listener){
                h->refs[b].r->v->unset_listener(h->refs[b].r, topic);
            }
            R->cb = NULL;
            R->user = NULL;
            return;
        }
    }
}

static int hub_capabilities(Replicator* rr){
    if (!rr) return 0;
    HubImpl* h = (HubImpl*)rr->impl;
    int caps = 0;
    for (int i=0;i<h->n;i++){
        int c = 0;
        if (h->refs[i].r && h->refs[i].r->v && h->refs[i].r->v->capabilities) c = h->refs[i].r->v->capabilities(h->refs[i].r);
        caps |= c;
    }
    return caps;
}

static int hub_health(Replicator* rr){
    if (!rr) return -1;
    HubImpl* h = (HubImpl*)rr->impl;
    /* Здоров, если есть хотя бы один здоровый бэкенд. */
    for (int i=0;i<h->n;i++){
        int hl = -1;
        if (h->refs[i].r && h->refs[i].r->v && h->refs[i].r->v->health) hl = h->refs[i].r->v->health(h->refs[i].r);
        if (hl == 0) return 0;
    }
    return -1;
}

static const ReplicatorVt HUB_VT = {
    .destroy      = hub_destroy,
    .publish      = hub_publish,
    .set_listener = hub_set_listener,
    .unset_listener = hub_unset_listener,
    .capabilities = hub_capabilities,
    .health       = hub_health,
};

Replicator* replicator_create_hub(const ReplBackendRef* backends, int n_backends,
                                  int required_caps, int adopt_backends){
    if (n_backends < 0) n_backends = 0;
    if (n_backends > HUB_MAX_BACKENDS) n_backends = HUB_MAX_BACKENDS;
    HubImpl* h = (HubImpl*)calloc(1, sizeof(HubImpl));
    if (!h) return NULL;
    for (int i=0;i<n_backends;i++) h->refs[i] = backends[i];
    h->n = n_backends;
    h->adopt = adopt_backends ? 1 : 0;
    h->required_caps = required_caps;
    /* Дефолты для всех потенциальных маршрутов */
    h->rn = 0;
    for (int i=0;i<HUB_MAX_TOPICS;i++){
        h->routes[i].have = 0;
        h->routes[i].bidx = -1;
        h->routes[i].cb = NULL; h->routes[i].user = NULL;
        h->routes[i].blocked = 0;
        h->routes[i].q = NULL; h->routes[i].qn = 0; h->routes[i].qcap = 0;
        h->routes[i].required_caps = required_caps;
        h->routes[i].forced_bidx   = -1;
    }
    Replicator* r = (Replicator*)calloc(1, sizeof(Replicator));
    if (!r){ free(h); return NULL; }
    r->v = &HUB_VT;
    r->impl = h;
    return r;
}



/* ===== мягкий свитч бэкенда ===== */
void replhub_switch_backend(Replicator* rr, TopicId topic, int to_backend){
    if (!rr) return;
    HubImpl* h = (HubImpl*)rr->impl; if (!h) return;
    /* найти/создать маршрут */
    int ri = -1;
    for (int i=0;i<h->rn;i++) if (h->routes[i].have && topic_eq(h->routes[i].topic, topic)){ ri=i; break; }
    if (ri < 0){
        if (h->rn >= HUB_MAX_TOPICS) return;
        ri = h->rn++;
        h->routes[ri].have=1; h->routes[ri].topic=topic; h->routes[ri].bidx=-1;
        h->routes[ri].cb=NULL; h->routes[ri].user=NULL;
        h->routes[ri].blocked=0; h->routes[ri].q=NULL; h->routes[ri].qn=0; h->routes[ri].qcap=0;
        h->routes[ri].required_caps = h->required_caps;
        h->routes[ri].forced_bidx   = -1;
    }
    TopicRoute* R = &h->routes[ri];
    /* блокируем публикации */
    R->blocked = 1;
    /* выбрать целевой бэкенд */
    int to = to_backend;
    if (to < 0 || to >= h->n){
        to = hub_choose(h, topic);
        if (to < 0){ R->blocked = 0; return; }
    }
    /* снапшот по типу (если доступен) */
    void* user = NULL;
    const TypeVt* vt = type_registry_get_default(topic.type_id, &user);
    if (vt && vt->snapshot){
        void* blob=NULL; size_t blen=0; uint32_t schema=0;
        if (vt->snapshot(user, &schema, &blob, &blen) == 0 && blob && blen>0){
            ConOp sop = (ConOp){0};
            sop.topic = topic;
            sop.console_id = topic.inst_id;
            sop.schema = schema;        // <--- теперь несём версию
            sop.tag = "snapshot";
            sop.init_blob = blob; sop.init_size = blen;
            replicator_publish(h->refs[to].r, &sop);
            free(blob);
        }
    }
    /* перевесить listener (если был) */
    {
        int from = R->bidx;
        if (R->cb && from>=0 &&
            h->refs[from].r && h->refs[from].r->v && h->refs[from].r->v->unset_listener)
        {
            h->refs[from].r->v->unset_listener(h->refs[from].r, topic);
        }
    }
    if (R->cb && h->refs[to].r && h->refs[to].r->v && h->refs[to].r->v->set_listener){
        h->refs[to].r->v->set_listener(h->refs[to].r, topic, hub_on_confirm, h);
    }
    R->bidx = to;
    /* разблокировать и дослать буфер */
    R->blocked = 0;
    if (R->bidx>=0 && h->refs[R->bidx].r && h->refs[R->bidx].r->v && h->refs[R->bidx].r->v->publish){
        for (int i=0;i<R->qn;i++){
            h->refs[R->bidx].r->v->publish(h->refs[R->bidx].r, &R->q[i].op);
            bufop_free(&R->q[i]);
        }
    }
    free(R->q); R->q=NULL; R->qn=R->qcap=0;
}

/* === Per-topic политика: требования по возможностям === */
void replhub_set_topic_caps(Replicator* rr, TopicId topic, int required_caps){
    if (!rr) return;
    HubImpl* h = (HubImpl*)rr->impl; if (!h) return;
    /* найти/создать маршрут */
    int ri = -1;
    for (int i=0;i<h->rn;i++) if (h->routes[i].have && topic_eq(h->routes[i].topic, topic)){ ri=i; break; }
    if (ri < 0){
        if (h->rn >= HUB_MAX_TOPICS) return;
        ri = h->rn++;
        h->routes[ri].have=1; h->routes[ri].topic=topic; h->routes[ri].bidx=-1;
        h->routes[ri].cb=NULL; h->routes[ri].user=NULL;
        h->routes[ri].blocked=0; h->routes[ri].q=NULL; h->routes[ri].qn=0; h->routes[ri].qcap=0;
        h->routes[ri].required_caps = h->required_caps;
        h->routes[ri].forced_bidx   = -1;
    }
    TopicRoute* R = &h->routes[ri];
    R->required_caps = required_caps;
    /* Перевыбор и перевешивание listener’а при необходимости */
    int want = hub_choose(h, topic);
    if (want != R->bidx){
        /* снять listener со старого бэкенда, если он был */
        int old = R->bidx;
        if (R->cb && old>=0 &&
            h->refs[old].r && h->refs[old].r->v && h->refs[old].r->v->unset_listener)
        {
            h->refs[old].r->v->unset_listener(h->refs[old].r, topic);
        }
        if (R->cb && want>=0 && h->refs[want].r && h->refs[want].r->v && h->refs[want].r->v->set_listener){
            h->refs[want].r->v->set_listener(h->refs[want].r, topic, hub_on_confirm, h);
        }
        R->bidx = want;
    }
}

/* === Per-topic политика: принудительный бэкенд (пин) === */
void replhub_force_backend(Replicator* rr, TopicId topic, int backend_index){
    if (!rr) return;
    HubImpl* h = (HubImpl*)rr->impl; if (!h) return;
    /* найти/создать маршрут */
    int ri = -1;
    for (int i=0;i<h->rn;i++) if (h->routes[i].have && topic_eq(h->routes[i].topic, topic)){ ri=i; break; }
    if (ri < 0){
        if (h->rn >= HUB_MAX_TOPICS) return;
        ri = h->rn++;
        h->routes[ri].have=1; h->routes[ri].topic=topic; h->routes[ri].bidx=-1;
        h->routes[ri].cb=NULL; h->routes[ri].user=NULL;
        h->routes[ri].blocked=0; h->routes[ri].q=NULL; h->routes[ri].qn=0; h->routes[ri].qcap=0;
        h->routes[ri].required_caps = h->required_caps;
        h->routes[ri].forced_bidx   = -1;
    }
    TopicRoute* R = &h->routes[ri];
    R->forced_bidx = backend_index; /* -1 снимает пин */
    /* Перевыбор и перевешивание listener’а при необходимости */
    int want = hub_choose(h, topic);
    if (want != R->bidx){
        /* снять listener со старого бэкенда, если он был */
        int old = R->bidx;
        if (R->cb && old>=0 &&
            h->refs[old].r && h->refs[old].r->v && h->refs[old].r->v->unset_listener)
        {
            h->refs[old].r->v->unset_listener(h->refs[old].r, topic);
        }
        if (R->cb && want>=0 && h->refs[want].r && h->refs[want].r->v && h->refs[want].r->v->set_listener){
            h->refs[want].r->v->set_listener(h->refs[want].r, topic, hub_on_confirm, h);
        }
        R->bidx = want;
    }
}


/* ===== Debug helpers ===== */
int replhub_debug_count_backends(Replicator* rr){
    if (!rr) return 0;
    HubImpl* h = (HubImpl*)rr->impl;
    return h ? h->n : 0;
}

int replhub_debug_backend_info(Replicator* rr, int idx, int* out_priority, int* out_caps, int* out_health){
    if (!rr) return -1;
    HubImpl* h = (HubImpl*)rr->impl; if (!h) return -1;
    if (idx < 0 || idx >= h->n) return -1;
    if (out_priority) *out_priority = h->refs[idx].priority;
    int caps = 0, health = -1;
    Replicator* r = h->refs[idx].r;
    if (r && r->v && r->v->capabilities) caps = r->v->capabilities(r);
    if (r && r->v && r->v->health)       health = r->v->health(r);
    if (out_caps)   *out_caps = caps;
    if (out_health) *out_health = health;
    return 0;
}

int replhub_debug_route_backend(Replicator* rr, TopicId topic){
    if (!rr) return -1;
    HubImpl* h = (HubImpl*)rr->impl; if (!h) return -1;
    for (int i=0;i<h->rn;i++){
        if (h->routes[i].have && topic_eq(h->routes[i].topic, topic)){
            return h->routes[i].bidx;
        }
    }
    return -1;
}

Replicator* replhub_debug_get_backend(Replicator* rr, int idx){
    if (!rr) return NULL;
    HubImpl* h = (HubImpl*)rr->impl; if (!h) return NULL;
    if (idx < 0 || idx >= h->n) return NULL;
    return h->refs[idx].r;
}
