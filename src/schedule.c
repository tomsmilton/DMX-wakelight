#include "schedule.h"

#include "cJSON.h"
#include "dismiss.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "sched";
static const char *NVS_NS = "wakelight";
static const char *NVS_KEY = "schedule";

static schedule_t g_cur;
static SemaphoreHandle_t g_lock;

static void default_schedule(schedule_t *s) {
  memset(s, 0, sizeof(*s));
  s->enabled = false;
  s->count = 3;
  s->points[0] = (waypoint_t){.minute_of_day = 6 * 60 + 30, .brightness_pct = 0,   .cct_k = 2500};
  s->points[1] = (waypoint_t){.minute_of_day = 6 * 60 + 50, .brightness_pct = 30,  .cct_k = 3000};
  s->points[2] = (waypoint_t){.minute_of_day = 7 * 60 + 0,  .brightness_pct = 100, .cct_k = 5000};
}

static void sort_points(schedule_t *s) {
  for (int i = 1; i < s->count; i++) {
    waypoint_t w = s->points[i];
    int j = i - 1;
    while (j >= 0 && s->points[j].minute_of_day > w.minute_of_day) {
      s->points[j + 1] = s->points[j];
      j--;
    }
    s->points[j + 1] = w;
  }
}

static void clamp_point(waypoint_t *w) {
  if (w->minute_of_day > 1439) w->minute_of_day = 1439;
  if (w->brightness_pct > 100) w->brightness_pct = 100;
  if (w->cct_k < 2500) w->cct_k = 2500;
  if (w->cct_k > 10000) w->cct_k = 10000;
}

bool schedule_load(schedule_t *out) {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();

  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    default_schedule(out);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_cur = *out;
    xSemaphoreGive(g_lock);
    return false;
  }
  size_t len = 0;
  esp_err_t err = nvs_get_blob(h, NVS_KEY, NULL, &len);
  if (err != ESP_OK || len == 0 || len > 2048) {
    nvs_close(h);
    default_schedule(out);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_cur = *out;
    xSemaphoreGive(g_lock);
    return false;
  }
  char *buf = malloc(len + 1);
  if (!buf) { nvs_close(h); default_schedule(out); return false; }
  nvs_get_blob(h, NVS_KEY, buf, &len);
  buf[len] = 0;
  nvs_close(h);

  bool ok = schedule_from_json(buf, len, out);
  free(buf);
  if (!ok) {
    ESP_LOGW(TAG, "stored schedule invalid; using default");
    default_schedule(out);
  }
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_cur = *out;
  xSemaphoreGive(g_lock);
  return ok;
}

bool schedule_save(schedule_t *s) {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();
  if (s->count > SCHEDULE_MAX_POINTS) return false;
  for (int i = 0; i < s->count; i++) clamp_point(&s->points[i]);
  sort_points(s);

  char buf[1024];
  int n = schedule_to_json(s, buf, sizeof(buf));
  if (n <= 0) return false;

  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_blob(h, NVS_KEY, buf, n);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs save failed: %d", err);
    return false;
  }

  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_cur = *s;
  xSemaphoreGive(g_lock);

  // If the new schedule's first waypoint is later today, drop any "done for
  // today" — saving a forward-looking schedule is the user's signal they
  // want it to fire.
  if (s->enabled && s->count > 0) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    uint16_t mod_now = lt.tm_hour * 60 + lt.tm_min;
    if (s->points[0].minute_of_day > mod_now) dismiss_reset();
  }
  return true;
}

void schedule_get(schedule_t *out) {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();
  xSemaphoreTake(g_lock, portMAX_DELAY);
  *out = g_cur;
  xSemaphoreGive(g_lock);
}

// Interpolation is done in DMX-byte space (0-255) for brightness instead of
// whole percents, so each step through the ramp is ~0.4% instead of the
// 1%-that-maps-to-2-3-DMX-units jumps you get when interpolating in percent.
static uint16_t pct_to_b255(uint8_t pct) {
  if (pct >= 100) return 255;
  return (uint16_t)((uint32_t)pct * 255 / 100);
}

bool schedule_eval(const schedule_t *s, uint32_t sod,
                   uint8_t *brightness_byte, uint16_t *cct_k) {
  if (!s->enabled || s->count == 0) return false;
  uint32_t first_sod = (uint32_t)s->points[0].minute_of_day * 60;
  uint32_t last_sod  = (uint32_t)s->points[s->count - 1].minute_of_day * 60;
  if (sod < first_sod) return false;
  if (sod >= last_sod) {
    // Hold at the last waypoint until the schedule is disabled or reshaped.
    *brightness_byte = (uint8_t)pct_to_b255(s->points[s->count - 1].brightness_pct);
    *cct_k = s->points[s->count - 1].cct_k;
    return true;
  }
  for (int i = 0; i + 1 < s->count; i++) {
    const waypoint_t *a = &s->points[i];
    const waypoint_t *b = &s->points[i + 1];
    uint32_t a_sod = (uint32_t)a->minute_of_day * 60;
    uint32_t b_sod = (uint32_t)b->minute_of_day * 60;
    if (sod >= a_sod && sod < b_sod) {
      uint32_t span = b_sod - a_sod;
      uint32_t pos = sod - a_sod;
      int32_t ab = pct_to_b255(a->brightness_pct);
      int32_t bb = pct_to_b255(b->brightness_pct);
      int32_t dc = (int32_t)b->cct_k - (int32_t)a->cct_k;
      *brightness_byte = (uint8_t)(ab + ((bb - ab) * (int32_t)pos) / (int32_t)span);
      *cct_k = (uint16_t)(a->cct_k + (dc * (int32_t)pos) / (int32_t)span);
      return true;
    }
  }
  return false;
}

int schedule_to_json(const schedule_t *s, char *buf, size_t buflen) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "enabled", s->enabled);
  cJSON *arr = cJSON_AddArrayToObject(root, "points");
  for (int i = 0; i < s->count; i++) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "m", s->points[i].minute_of_day);
    cJSON_AddNumberToObject(p, "b", s->points[i].brightness_pct);
    cJSON_AddNumberToObject(p, "k", s->points[i].cct_k);
    cJSON_AddItemToArray(arr, p);
  }
  bool ok = cJSON_PrintPreallocated(root, buf, buflen, 0);
  cJSON_Delete(root);
  return ok ? (int)strlen(buf) : -1;
}

bool schedule_from_json(const char *json, size_t len, schedule_t *out) {
  (void)len;
  cJSON *root = cJSON_Parse(json);
  if (!root) return false;
  memset(out, 0, sizeof(*out));
  cJSON *en = cJSON_GetObjectItem(root, "enabled");
  out->enabled = cJSON_IsTrue(en);
  cJSON *arr = cJSON_GetObjectItem(root, "points");
  bool ok = cJSON_IsArray(arr);
  if (ok) {
    int n = cJSON_GetArraySize(arr);
    if (n > SCHEDULE_MAX_POINTS) n = SCHEDULE_MAX_POINTS;
    out->count = n;
    for (int i = 0; i < n; i++) {
      cJSON *p = cJSON_GetArrayItem(arr, i);
      cJSON *m = cJSON_GetObjectItem(p, "m");
      cJSON *b = cJSON_GetObjectItem(p, "b");
      cJSON *k = cJSON_GetObjectItem(p, "k");
      if (!cJSON_IsNumber(m) || !cJSON_IsNumber(b) || !cJSON_IsNumber(k)) {
        ok = false;
        break;
      }
      out->points[i].minute_of_day = (uint16_t)m->valueint;
      out->points[i].brightness_pct = (uint8_t)b->valueint;
      out->points[i].cct_k = (uint16_t)k->valueint;
      clamp_point(&out->points[i]);
    }
    if (ok) sort_points(out);
  }
  cJSON_Delete(root);
  return ok;
}
