#pragma once
#include <stdint.h>
#define FRAME_MS 16u
static inline uint32_t next_frame(uint32_t now){ return now + FRAME_MS; }
