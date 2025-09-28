#pragma once
#include "window.h"

#define MAX_DAMAGE 64
typedef struct {
    Rect r[MAX_DAMAGE];
    int n;
} DamageList;

static inline void damage_init(DamageList *d){ d->n=0; }
static inline void damage_clear(DamageList *d){ d->n=0; }
static inline int  damage_count(const DamageList *d){ return d->n; }
static inline Rect damage_at(const DamageList *d, int i){ return d->r[i]; }
static inline void damage_add(DamageList *d, Rect rc){
    if (rc.w<=0 || rc.h<=0) return;
    if (d->n < MAX_DAMAGE) d->r[d->n++] = rc;
}
