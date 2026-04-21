#include "device_id.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "devid";
static const char *NVS_NS = "wakelight";
static const char *NVS_KEY_NAME = "name";

#define NAME_MAX 48
#define HOST_MAX 32

static char g_name[NAME_MAX + 1];
static char g_slug[HOST_MAX + 1];
static char g_default_host[HOST_MAX + 1];
static char g_chosen[HOST_MAX + 1];
static bool g_holds_wakelight = false;

static void slugify(const char *in, char *out, size_t out_cap) {
  size_t j = 0;
  bool last_dash = true;
  for (size_t i = 0; in[i] && j + 1 < out_cap; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (keep) { out[j++] = (char)c; last_dash = false; }
    else if (!last_dash) { out[j++] = '-'; last_dash = true; }
  }
  while (j > 0 && out[j - 1] == '-') j--;
  out[j] = 0;
}

static void compute_default_host(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(g_default_host, sizeof(g_default_host), "wakelight-%02x%02x", mac[4], mac[5]);
}

static void recompute_slug(void) {
  char s[HOST_MAX + 1];
  slugify(g_name, s, sizeof(s));
  if (s[0]) {
    strncpy(g_slug, s, sizeof(g_slug) - 1);
  } else {
    strncpy(g_slug, g_default_host, sizeof(g_slug) - 1);
  }
  g_slug[sizeof(g_slug) - 1] = 0;
}

void device_id_init(void) {
  compute_default_host();
  g_name[0] = 0;

  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    size_t len = sizeof(g_name);
    if (nvs_get_str(h, NVS_KEY_NAME, g_name, &len) != ESP_OK) g_name[0] = 0;
    nvs_close(h);
  }
  if (!g_name[0]) {
    strncpy(g_name, g_default_host, sizeof(g_name) - 1);
    g_name[sizeof(g_name) - 1] = 0;
  }
  recompute_slug();
  // Until the mDNS layer probes and reports back, assume we'll get our slug.
  strncpy(g_chosen, g_slug, sizeof(g_chosen) - 1);
  g_chosen[sizeof(g_chosen) - 1] = 0;
  ESP_LOGI(TAG, "name=\"%s\" slug=%s default=%s", g_name, g_slug, g_default_host);
}

const char *device_id_name(void) { return g_name; }
const char *device_id_slug(void) { return g_slug; }
const char *device_id_default_hostname(void) { return g_default_host; }
const char *device_id_chosen_hostname(void) { return g_chosen; }
bool device_id_holds_wakelight(void) { return g_holds_wakelight; }

void device_id_set_chosen_hostname(const char *host) {
  if (!host) return;
  strncpy(g_chosen, host, sizeof(g_chosen) - 1);
  g_chosen[sizeof(g_chosen) - 1] = 0;
}
void device_id_set_holds_wakelight(bool v) { g_holds_wakelight = v; }

void device_id_slug_for(const char *name, char *out, size_t out_cap) {
  slugify(name ? name : "", out, out_cap);
  if (!out[0]) {
    strncpy(out, g_default_host, out_cap - 1);
    out[out_cap - 1] = 0;
  }
}

bool device_id_set_name(const char *name) {
  if (!name) return false;
  size_t len = strnlen(name, NAME_MAX + 1);
  if (len == 0 || len > NAME_MAX) return false;
  strncpy(g_name, name, sizeof(g_name) - 1);
  g_name[sizeof(g_name) - 1] = 0;
  recompute_slug();

  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_str(h, NVS_KEY_NAME, g_name);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "persist failed: %d", err);
    return false;
  }
  ESP_LOGI(TAG, "name=\"%s\" slug=%s", g_name, g_slug);
  return true;
}
