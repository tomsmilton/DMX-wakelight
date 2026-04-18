#pragma once

typedef enum {
  OVERRIDE_AUTO = 0,  // follow schedule
  OVERRIDE_ON   = 1,  // force lamp on
  OVERRIDE_OFF  = 2,  // force lamp off
} override_mode_t;

void override_set(override_mode_t m);
override_mode_t override_get(void);
const char *override_name(override_mode_t m);
