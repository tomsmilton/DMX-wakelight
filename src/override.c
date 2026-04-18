#include "override.h"

#include <stdatomic.h>

// Pack: bits 0-1 mode, 2-9 brightness_pct, 10-23 cct_k (14 bits, fits 0-16383),
// 24-31 gm_byte. Everything in one atomic uint32.
static _Atomic uint32_t g_state = ((uint32_t)128 << 24);  // neutral G/M default

static inline uint32_t pack(override_mode_t m, uint8_t b, uint16_t k, uint8_t gm) {
  return ((uint32_t)(m & 0x3))
       | (((uint32_t)b & 0xFF) << 2)
       | (((uint32_t)k & 0x3FFF) << 10)
       | ((uint32_t)gm << 24);
}

static inline void unpack(uint32_t s, uint8_t *b, uint16_t *k, uint8_t *gm) {
  if (b)  *b  = (s >> 2) & 0xFF;
  if (k)  *k  = (s >> 10) & 0x3FFF;
  if (gm) *gm = (s >> 24) & 0xFF;
}

void override_set(override_mode_t m) {
  uint32_t s = atomic_load(&g_state);
  uint8_t b = 0; uint16_t k = 0; uint8_t gm = 128;
  unpack(s, &b, &k, &gm);
  atomic_store(&g_state, pack(m, b, k, gm));
}

override_mode_t override_get(void) {
  return (override_mode_t)(atomic_load(&g_state) & 0x3);
}

const char *override_name(override_mode_t m) {
  switch (m) {
    case OVERRIDE_ON:     return "on";
    case OVERRIDE_OFF:    return "off";
    case OVERRIDE_MANUAL: return "manual";
    default:              return "auto";
  }
}

void override_set_manual(uint8_t brightness_pct, uint16_t cct_k, uint8_t gm_byte) {
  if (brightness_pct > 100) brightness_pct = 100;
  if (cct_k < 2500) cct_k = 2500;
  if (cct_k > 10000) cct_k = 10000;
  atomic_store(&g_state, pack(OVERRIDE_MANUAL, brightness_pct, cct_k, gm_byte));
}

void override_get_manual(uint8_t *brightness_pct, uint16_t *cct_k, uint8_t *gm_byte) {
  uint32_t s = atomic_load(&g_state);
  unpack(s, brightness_pct, cct_k, gm_byte);
}
