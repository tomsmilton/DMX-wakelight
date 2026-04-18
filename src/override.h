#pragma once

#include <stdint.h>

typedef enum {
  OVERRIDE_AUTO   = 0,  // follow schedule
  OVERRIDE_ON     = 1,  // force lamp on (default brightness/CCT)
  OVERRIDE_OFF    = 2,  // force lamp off
  OVERRIDE_MANUAL = 3,  // follow manual brightness/CCT below
} override_mode_t;

void override_set(override_mode_t m);
override_mode_t override_get(void);
const char *override_name(override_mode_t m);

// Used only when mode is MANUAL. Values are clamped by consumers.
// gm_byte: 0-255 DMX G/M tint, 128 = neutral.
void override_set_manual(uint8_t brightness_pct, uint16_t cct_k, uint8_t gm_byte);
void override_get_manual(uint8_t *brightness_pct, uint16_t *cct_k, uint8_t *gm_byte);
