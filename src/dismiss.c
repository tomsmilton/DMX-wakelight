#include "dismiss.h"

#include "esp_log.h"
#include "nvs.h"

#include <stdatomic.h>
#include <time.h>

static const char *TAG = "dismiss";
static const char *NVS_NS  = "wakelight";
static const char *NVS_KEY = "dism_ord";

// Year/month/day packed into one int. Sentinel 0 = never dismissed.
static _Atomic int g_dismissed_ord = 0;

static int ord_today(void) {
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  return (lt.tm_year + 1900) * 512 + (lt.tm_mon + 1) * 32 + lt.tm_mday;
}

void dismiss_init(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  int32_t stored = 0;
  if (nvs_get_i32(h, NVS_KEY, &stored) == ESP_OK) {
    atomic_store(&g_dismissed_ord, (int)stored);
  }
  nvs_close(h);
}

void dismiss_for_today(void) {
  int d = ord_today();
  atomic_store(&g_dismissed_ord, d);

  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "nvs open failed; dismiss in RAM only");
    return;
  }
  esp_err_t err = nvs_set_i32(h, NVS_KEY, (int32_t)d);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) ESP_LOGW(TAG, "nvs write failed: %d", err);
}

void dismiss_reset(void) {
  atomic_store(&g_dismissed_ord, 0);
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, NVS_KEY);
  nvs_commit(h);
  nvs_close(h);
}

bool dismiss_is_active(void) {
  int d = atomic_load(&g_dismissed_ord);
  if (d == 0) return false;
  return d == ord_today();
}
