#pragma once
#include <stdint.h>
extern uint32_t test_clock;
static inline uint32_t k_uptime_get_32(void){return test_clock;}
