/* Консольный слой: надстройка поверх общих типов ConOp/ConOpType/ConPosId.
 * Здесь остаётся только то, что специфично для консоли (например, ConItemId).
 * Общие типы вынесены в common/conop.h, чтобы net/ и replication/ не зависели от console/.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
/* Базовые типы операций */
#include "common/conop.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* ======================= Идентификаторы элементов ======================= */
    typedef uint64_t ConItemId;

#ifndef CON_ITEMID_INVALID
#  define CON_ITEMID_INVALID ((ConItemId)0)
#endif


#ifdef __cplusplus
} /* extern "C" */
#endif
