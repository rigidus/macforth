#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

    void     global_set_color(uint32_t argb);
    uint32_t global_get_color(void);

#ifdef __cplusplus
}
#endif
