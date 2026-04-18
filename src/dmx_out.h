#pragma once

#include <stdint.h>

// Start the DMX driver + continuous sender task.
void dmx_out_start(void);

// brightness_byte: 0-255 (DMX scale); cct_k: 2500-10000; gm_byte: 0-255
// (128 = neutral, 0 = full green, 255 = full magenta). Thread-safe.
void dmx_out_set(uint8_t brightness_byte, uint16_t cct_k, uint8_t gm_byte);

// Force the fixture dark without touching the last desired setpoint.
void dmx_out_off(void);
