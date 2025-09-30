#pragma once
#include <stdint.h>
/* ВЕРСИОНИРОВАННЫЕ ДЕЛЬТЫ ВИДЖЕТОВ (под CRDT/LWW)
   Заголовок обязателен для tag="cw.delta".
   Правило применения: LWW по (hlc, actor_id) — более "новая" дельта побеждает, коммутативно. */

/* 'CDV1' (Console Delta V1) в LE */
#define CON_DELTA_SCHEMA_V1  ((uint32_t)0x31564443u)

typedef enum {
    CON_DELTA_KIND_LWW_SET = 1  /* полный LWW-set состояния виджета */
} ConDeltaKind;

typedef struct {
    uint32_t schema;     /* CON_DELTA_SCHEMA_V1 */
    uint16_t kind;       /* ConDeltaKind */
    uint16_t flags;      /* зарезервировано */
    uint64_t hlc;        /* Hybrid Logical Clock отправителя */
    uint32_t actor_id;   /* идентификатор узла/актора (для tie-break) */
    uint32_t reserved;   /* выравнивание/запас под будущее */
} ConDeltaHdr;
