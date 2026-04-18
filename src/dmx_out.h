#pragma once

#include <stdint.h>

// Start the DMX driver + continuous sender task.
void dmx_out_start(void);

// brightness_pct: 0-100; cct_k: 2500-10000. Thread-safe (atomic snapshot).
void dmx_out_set(uint8_t brightness_pct, uint16_t cct_k);

// Force the fixture dark without touching the last desired setpoint.
void dmx_out_off(void);
