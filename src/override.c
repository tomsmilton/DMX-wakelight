#include "override.h"

#include <stdatomic.h>

static _Atomic int g_mode = OVERRIDE_AUTO;

void override_set(override_mode_t m) { atomic_store(&g_mode, (int)m); }
override_mode_t override_get(void) { return (override_mode_t)atomic_load(&g_mode); }

const char *override_name(override_mode_t m) {
  switch (m) {
    case OVERRIDE_ON:  return "on";
    case OVERRIDE_OFF: return "off";
    default:           return "auto";
  }
}
