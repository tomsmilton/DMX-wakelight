#include "schedule.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

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
  return true;
}

void schedule_get(schedule_t *out) {
  if (!g_lock) g_lock = xSemaphoreCreateMutex();
  xSemaphoreTake(g_lock, portMAX_DELAY);
  *out = g_cur;
  xSemaphoreGive(g_lock);
}

bool schedule_eval(const schedule_t *s, uint16_t mod,
                   uint8_t *brightness_pct, uint16_t *cct_k) {
  if (!s->enabled || s->count == 0) return false;
  if (mod < s->points[0].minute_of_day) return false;
  if (mod >= s->points[s->count - 1].minute_of_day) {
    *brightness_pct = s->points[s->count - 1].brightness_pct;
    *cct_k = s->points[s->count - 1].cct_k;
    // After last waypoint, lamp is "done" — caller decides whether to hold
    // or switch off. We return false to mean "outside active ramp".
    return false;
  }
  // Find bracketing pair.
  for (int i = 0; i + 1 < s->count; i++) {
    const waypoint_t *a = &s->points[i];
    const waypoint_t *b = &s->points[i + 1];
    if (mod >= a->minute_of_day && mod < b->minute_of_day) {
      uint32_t span = b->minute_of_day - a->minute_of_day;
      uint32_t pos = mod - a->minute_of_day;
      int32_t db = (int32_t)b->brightness_pct - (int32_t)a->brightness_pct;
      int32_t dc = (int32_t)b->cct_k - (int32_t)a->cct_k;
      *brightness_pct = (uint8_t)(a->brightness_pct + (db * (int32_t)pos) / (int32_t)span);
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
