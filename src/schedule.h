#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHEDULE_MAX_POINTS 10

typedef struct {
  uint16_t minute_of_day;  // 0..1439
  uint8_t brightness_pct;  // 0..100
  uint16_t cct_k;          // 2500..10000
} waypoint_t;

typedef struct {
  bool enabled;
  uint8_t count;           // 0..SCHEDULE_MAX_POINTS
  waypoint_t points[SCHEDULE_MAX_POINTS];
} schedule_t;

// Load from NVS into `out`. Returns false if no stored schedule (out is seeded
// with a sensible default in that case).
bool schedule_load(schedule_t *out);

// Validate, sort by minute_of_day, persist to NVS. Returns true on success.
bool schedule_save(schedule_t *s);

// Get a snapshot of the currently active schedule (thread-safe copy).
void schedule_get(schedule_t *out);

// Compute interpolated output for a given minute-of-day.
// Returns true if inside the ramp window (first..last waypoint); false means
// lamp should be off. When inside, *brightness_pct and *cct_k are filled.
bool schedule_eval(const schedule_t *s, uint16_t minute_of_day,
                   uint8_t *brightness_pct, uint16_t *cct_k);

// Serialize/deserialize to JSON. `buf` must be big enough (~1KB is plenty).
int schedule_to_json(const schedule_t *s, char *buf, size_t buflen);
bool schedule_from_json(const char *json, size_t len, schedule_t *out);
