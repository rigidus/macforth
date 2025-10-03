#include "replication/type_registry.h"
#include <stdlib.h>
#include <string.h>

typedef struct Entry {
    uint64_t      type_id;
    const TypeVt* vt;
    void*         user;
} Entry;

struct TypeRegistry {
    Entry* e;
    int    n;
    int    cap;
};

static struct TypeRegistry g_def; /* дефолтный singleton */

TypeRegistry* type_registry_default(void){ return &g_def; }

void type_registry_reset(TypeRegistry* r){
    if (!r) return;
    free(r->e);
    r->e = NULL; r->n = r->cap = 0;
}

static int ensure_cap(TypeRegistry* r, int want){
    if (r->cap >= want) return 1;
    int ncap = r->cap ? r->cap*2 : 8;
    while (ncap < want) ncap *= 2;
    void* nb = realloc(r->e, (size_t)ncap * sizeof(Entry));
    if (!nb) return 0;
    r->e = (Entry*)nb; r->cap = ncap; return 1;
}

int type_registry_register(TypeRegistry* r, uint64_t type_id, const TypeVt* vt, void* user){
    if (!r || !vt || !vt->apply) return -1;
    /* если уже есть — обновим */
    for (int i=0;i<r->n;i++){
        if (r->e[i].type_id == type_id){
            r->e[i].vt = vt;
            r->e[i].user = user;
            return 0;
        }
    }
    if (!ensure_cap(r, r->n+1)) return -1;
    r->e[r->n++] = (Entry){ type_id, vt, user };
    return 0;
}

const TypeVt* type_registry_get(TypeRegistry* r, uint64_t type_id, void** out_user){
    if (!r) return NULL;
    for (int i=0;i<r->n;i++){
        if (r->e[i].type_id == type_id){
            if (out_user) *out_user = r->e[i].user;
            return r->e[i].vt;
        }
    }
    if (out_user) *out_user = NULL;
    return NULL;
}
