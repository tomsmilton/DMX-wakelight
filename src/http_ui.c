#include "http_ui.h"

#include "cJSON.h"
#include "device_id.h"
#include "dismiss.h"
#include "dmx_out.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "override.h"
#include "schedule.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "http";

extern const unsigned char index_html_data[];
extern const size_t index_html_len;
extern const unsigned char live_html_data[];
extern const size_t live_html_len;
extern const unsigned char picker_html_data[];
extern const size_t picker_html_len;

static esp_err_t send_gz_html(httpd_req_t *req, const unsigned char *buf, size_t len) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char *)buf, len);
}

// `/` serves the picker only when the request came in via the wakelight.local
// delegate hostname (i.e. the device's own slug isn't literally "wakelight").
// All other accesses — slug.local, default-host.local, IP — get the schedule
// page as before.
static bool host_is_delegate(httpd_req_t *req) {
  if (strcmp(device_id_chosen_hostname(), "wakelight") == 0) return false;
  size_t need = httpd_req_get_hdr_value_len(req, "Host");
  if (need == 0 || need > 64) return false;
  char host[65];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
  char *colon = strchr(host, ':');
  if (colon) *colon = 0;
  return strcasecmp(host, "wakelight.local") == 0;
}

static esp_err_t root_get(httpd_req_t *req) {
  if (host_is_delegate(req)) {
    return send_gz_html(req, picker_html_data, picker_html_len);
  }
  return send_gz_html(req, index_html_data, index_html_len);
}
static esp_err_t live_get(httpd_req_t *req) { return send_gz_html(req, live_html_data, live_html_len); }

static esp_err_t schedule_get_h(httpd_req_t *req) {
  schedule_t s;
  schedule_get(&s);
  char buf[1024];
  int n = schedule_to_json(&s, buf, sizeof(buf));
  if (n <= 0) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, n);
}

static esp_err_t schedule_put_h(httpd_req_t *req) {
  if (req->content_len > 2048) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too big");
  }
  char body[2048];
  int total = 0;
  while (total < req->content_len) {
    int r = httpd_req_recv(req, body + total, req->content_len - total);
    if (r <= 0) return httpd_resp_send_500(req);
    total += r;
  }
  body[total] = 0;

  schedule_t s;
  if (!schedule_from_json(body, total, &s)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
  }
  if (!schedule_save(&s)) {
    return httpd_resp_send_500(req);
  }
  return schedule_get_h(req);
}

static esp_err_t status_get_h(httpd_req_t *req) {
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  uint16_t mod = lt.tm_hour * 60 + lt.tm_min;
  uint32_t sod = (uint32_t)lt.tm_hour * 3600 + (uint32_t)lt.tm_min * 60 + (uint32_t)lt.tm_sec;

  schedule_t s;
  schedule_get(&s);
  override_mode_t ov = override_get();
  bool dism = dismiss_is_active();

  // Mirror the ramp task's logic so the UI sees what actually goes to DMX.
  uint8_t byte = 0;
  uint16_t k = 2700;
  uint8_t gm_byte = 128;
  bool active = false;
  const char *state = "idle";
  uint32_t first_sod = s.count ? (uint32_t)s.points[0].minute_of_day * 60 : 0;
  uint32_t last_sod  = s.count ? (uint32_t)s.points[s.count - 1].minute_of_day * 60 : 0;
  if (now < 1700000000) state = "no_time";
  else if (ov == OVERRIDE_ON) { byte = 255; k = 4000; active = true; state = "on"; }
  else if (ov == OVERRIDE_OFF) { byte = 0; k = 2700; state = "off"; }
  else if (ov == OVERRIDE_MANUAL) {
    uint8_t mp = 0;
    override_get_manual(&mp, &k, &gm_byte);
    byte = (mp >= 100) ? 255 : (uint8_t)((uint32_t)mp * 255 / 100);
    active = true;
    state = "manual";
  } else if (dism) {
    byte = 0; k = 2700; state = "dismissed";
  } else if (!s.enabled || s.count == 0 || sod < first_sod) {
    state = "idle";
  } else if (sod >= last_sod) {
    schedule_eval(&s, sod, &byte, &k);
    active = true;
    state = "holding";
  } else {
    active = schedule_eval(&s, sod, &byte, &k);
    state = active ? "ramping" : "idle";
    if (!active) { byte = 0; k = 2700; }
  }
  uint8_t pct = (uint8_t)((uint32_t)byte * 100 / 255);
  // Convert gm byte (0-255, 128 neutral) back to slider scale -100..+100.
  int gm_slider = ((int)gm_byte * 200 / 255) - 100;

  cJSON *root = cJSON_CreateObject();
  char tbuf[32];
  strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);
  cJSON_AddStringToObject(root, "now", tbuf);
  cJSON_AddNumberToObject(root, "mod", mod);
  cJSON_AddBoolToObject(root, "time_valid", now > 1700000000);
  cJSON_AddBoolToObject(root, "active", active);
  cJSON_AddNumberToObject(root, "brightness_pct", pct);
  cJSON_AddNumberToObject(root, "cct_k", k);
  cJSON_AddStringToObject(root, "override", override_name(ov));
  cJSON_AddBoolToObject(root, "dismissed", dism);
  cJSON_AddNumberToObject(root, "gm", gm_slider);
  cJSON_AddStringToObject(root, "state", state);

  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  httpd_resp_set_type(req, "application/json");
  esp_err_t r = httpd_resp_send(req, out, strlen(out));
  free(out);
  return r;
}

static esp_err_t override_post_h(httpd_req_t *req) {
  char body[64];
  int n = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
  int total = 0;
  while (total < n) {
    int r = httpd_req_recv(req, body + total, n - total);
    if (r <= 0) return httpd_resp_send_500(req);
    total += r;
  }
  body[total] = 0;
  cJSON *j = cJSON_Parse(body);
  if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
  cJSON *m = cJSON_GetObjectItem(j, "mode");
  override_mode_t ov = OVERRIDE_AUTO;
  if (cJSON_IsString(m)) {
    if (strcmp(m->valuestring, "on") == 0) ov = OVERRIDE_ON;
    else if (strcmp(m->valuestring, "off") == 0) ov = OVERRIDE_OFF;
  }
  cJSON_Delete(j);
  override_set(ov);
  ESP_LOGI(TAG, "override -> %s", override_name(ov));
  httpd_resp_set_type(req, "application/json");
  char out[48];
  int len = snprintf(out, sizeof(out), "{\"mode\":\"%s\"}", override_name(ov));
  return httpd_resp_send(req, out, len);
}

// WebSocket for live slider control. On open we switch to MANUAL; on socket
// close (clean close frame OR dropped connection) we revert to AUTO.
// The close hook below handles the drop case.
#define MAX_WS_FDS 4
static int g_ws_fds[MAX_WS_FDS];
static int g_ws_count = 0;

static void ws_add_fd(int fd) {
  for (int i = 0; i < MAX_WS_FDS; i++) if (g_ws_fds[i] == fd) return;
  for (int i = 0; i < MAX_WS_FDS; i++) {
    if (g_ws_fds[i] == 0) { g_ws_fds[i] = fd; g_ws_count++; return; }
  }
}

static void ws_remove_fd(int fd) {
  for (int i = 0; i < MAX_WS_FDS; i++) {
    if (g_ws_fds[i] == fd) {
      g_ws_fds[i] = 0;
      if (g_ws_count > 0) g_ws_count--;
      break;
    }
  }
  // Keep MANUAL state across WS drop — user must explicitly hit Auto/On/Off
  // on the schedule page to leave manual.
}

static esp_err_t live_ws_h(httpd_req_t *req) {
  int fd = httpd_req_to_sockfd(req);
  if (req->method == HTTP_GET) {
    ws_add_fd(fd);
    ESP_LOGI(TAG, "ws open fd=%d (clients=%d)", fd, g_ws_count);
    // Don't change state yet — only flip to MANUAL when a slider message
    // actually arrives. Keeps reconnects from flashing the lamp.
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {0};
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) return err;
  if (frame.len == 0 || frame.len > 128) return ESP_OK;

  uint8_t buf[129];
  frame.payload = buf;
  err = httpd_ws_recv_frame(req, &frame, sizeof(buf) - 1);
  if (err != ESP_OK) return err;
  buf[frame.len] = 0;

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    ws_remove_fd(fd);
    return ESP_OK;
  }

  if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

  cJSON *j = cJSON_Parse((const char *)buf);
  if (!j) return ESP_OK;
  cJSON *b = cJSON_GetObjectItem(j, "b");
  cJSON *k = cJSON_GetObjectItem(j, "k");
  cJSON *gm = cJSON_GetObjectItem(j, "gm");  // -100..+100, 0 = neutral
  if (cJSON_IsNumber(b) && cJSON_IsNumber(k)) {
    uint8_t bp = (uint8_t)b->valueint;
    uint16_t kk = (uint16_t)k->valueint;
    int gm_val = cJSON_IsNumber(gm) ? gm->valueint : 0;
    if (gm_val < -100) gm_val = -100;
    if (gm_val >  100) gm_val =  100;
    uint8_t gm_byte = (uint8_t)((gm_val + 100) * 255 / 200);
    override_set_manual(bp, kk, gm_byte);
    uint8_t out_byte = (bp >= 100) ? 255 : (uint8_t)((uint32_t)bp * 255 / 100);
    dmx_out_set(out_byte, kk, gm_byte);
  }
  cJSON_Delete(j);
  return ESP_OK;
}

static esp_err_t dismiss_post_h(httpd_req_t *req) {
  // POST /api/dismiss — terminal for the local date. No undo path.
  dismiss_for_today();
  httpd_resp_set_type(req, "application/json");
  const char *r = "{\"active\":true}";
  return httpd_resp_send(req, r, strlen(r));
}

static esp_err_t device_get_h(httpd_req_t *req);
static esp_err_t device_put_h(httpd_req_t *req);
static esp_err_t peers_get_h(httpd_req_t *req);

static const httpd_uri_t uris[] = {
  {.uri = "/",              .method = HTTP_GET,  .handler = root_get},
  {.uri = "/live",          .method = HTTP_GET,  .handler = live_get},
  {.uri = "/api/schedule",  .method = HTTP_GET,  .handler = schedule_get_h},
  {.uri = "/api/schedule",  .method = HTTP_PUT,  .handler = schedule_put_h},
  {.uri = "/api/status",    .method = HTTP_GET,  .handler = status_get_h},
  {.uri = "/api/override",  .method = HTTP_POST, .handler = override_post_h},
  {.uri = "/api/dismiss",   .method = HTTP_POST, .handler = dismiss_post_h},
  {.uri = "/api/device",    .method = HTTP_GET,  .handler = device_get_h},
  {.uri = "/api/device",    .method = HTTP_PUT,  .handler = device_put_h},
  {.uri = "/api/peers",     .method = HTTP_GET,  .handler = peers_get_h},
  {.uri = "/ws/live",       .method = HTTP_GET,  .handler = live_ws_h, .is_websocket = true},
};

// Called by httpd when any socket closes (clean or dropped). We close the
// socket and, if it was one of our WS clients, fall back to AUTO.
static void ws_close_fn(httpd_handle_t hd, int fd) {
  (void)hd;
  ws_remove_fd(fd);
  close(fd);
}

static bool get_my_ip4(uint32_t *out) {
  esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!nif) return false;
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(nif, &ip) != ESP_OK) return false;
  *out = ip.ip.addr;
  return true;
}

// Returns true if someone else on the network already claims the given
// hostname. A 250 ms probe is enough for same-LAN mDNS — longer makes boot slow.
static bool hostname_taken(const char *host) {
  esp_ip4_addr_t tmp;
  esp_err_t r = mdns_query_a(host, 250, &tmp);
  return r == ESP_OK;
}

// Same as hostname_taken but ignores self-responses (e.g. when we already
// advertise the name). Used for rename clash detection.
static bool hostname_taken_by_other(const char *host) {
  esp_ip4_addr_t probed;
  if (mdns_query_a(host, 250, &probed) != ESP_OK) return false;
  uint32_t mine;
  if (!get_my_ip4(&mine)) return true;  // can't tell → treat as conflict
  return probed.addr != mine;
}

// Walk slug → slug-2 → slug-3 → … → default_host until we find a free name.
// Writes the winner into `out` (size HOST_BUF).
#define HOST_BUF 33
static void pick_hostname(char *out) {
  const char *slug = device_id_slug();
  const char *def = device_id_default_hostname();
  if (!hostname_taken(slug)) { snprintf(out, HOST_BUF, "%s", slug); return; }
  for (int n = 2; n <= 9; n++) {
    char cand[HOST_BUF];
    snprintf(cand, sizeof(cand), "%s-%d", slug, n);
    if (!hostname_taken(cand)) { snprintf(out, HOST_BUF, "%s", cand); return; }
  }
  snprintf(out, HOST_BUF, "%s", def);  // MAC-based, guaranteed unique
}

static void get_my_ipaddr(mdns_ip_addr_t *out) {
  memset(out, 0, sizeof(*out));
  uint32_t v4;
  if (!get_my_ip4(&v4)) return;
  out->addr.type = ESP_IPADDR_TYPE_V4;
  out->addr.u_addr.ip4.addr = v4;
  out->next = NULL;
}

// Opportunistic: if "wakelight.local" isn't taken (or we already hold it),
// claim it as a delegate so any device can land on a picker.
static void try_claim_wakelight(void) {
  if (strcmp(device_id_chosen_hostname(), "wakelight") == 0) {
    device_id_set_holds_wakelight(true);  // we already are wakelight.local
    return;
  }
  if (device_id_holds_wakelight()) return;
  if (hostname_taken("wakelight")) return;
  mdns_ip_addr_t ip;
  get_my_ipaddr(&ip);
  if (ip.addr.u_addr.ip4.addr == 0) return;
  esp_err_t r = mdns_delegate_hostname_add("wakelight", &ip);
  if (r == ESP_OK) {
    device_id_set_holds_wakelight(true);
    ESP_LOGI(TAG, "claimed wakelight.local");
  } else {
    ESP_LOGW(TAG, "delegate add failed: %d", r);
  }
}

// Refresh the TXT records on _wakelight._tcp so peer discovery sees the
// current friendly name + slug. Safe to call before service_add (no-op then).
static void update_wakelight_txt(void) {
  mdns_txt_item_t items[] = {
    {"name", (char *)device_id_name()},
    {"slug", (char *)device_id_slug()},
  };
  mdns_service_txt_set("_wakelight", "_tcp", items, sizeof(items) / sizeof(items[0]));
}

static void apply_mdns_identity(void) {
  char chosen[HOST_BUF];
  pick_hostname(chosen);
  mdns_hostname_set(chosen);
  mdns_instance_name_set(device_id_name());
  device_id_set_chosen_hostname(chosen);
  ESP_LOGI(TAG, "mdns: http://%s.local/ (name \"%s\")", chosen, device_id_name());
  update_wakelight_txt();
  try_claim_wakelight();
}

static void mdns_refresh_task(void *arg) {
  (void)arg;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));  // 5 min
    try_claim_wakelight();
  }
}

static void start_mdns(void) {
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mdns_init failed: %d", err);
    return;
  }
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  // _wakelight._tcp is our discovery vehicle: peers PTR-query it to build
  // the picker list. TXT carries the friendly name + slug; refreshed on
  // rename via update_wakelight_txt().
  mdns_service_add(NULL, "_wakelight", "_tcp", 80, NULL, 0);
  apply_mdns_identity();
  xTaskCreate(mdns_refresh_task, "mdns-refresh", 3072, NULL, 2, NULL);
}

// Look up a TXT key in an mdns_result_t's flat key/value arrays.
static const char *txt_lookup(const mdns_result_t *r, const char *key) {
  for (size_t i = 0; i < r->txt_count; i++) {
    if (r->txt[i].key && strcmp(r->txt[i].key, key) == 0) return r->txt[i].value;
  }
  return NULL;
}

static esp_err_t peers_get_h(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(root, "peers");

  // Always list self first so the picker has at least one entry even if
  // the mDNS browse hasn't propagated yet.
  cJSON *me = cJSON_CreateObject();
  cJSON_AddStringToObject(me, "name", device_id_name());
  cJSON_AddStringToObject(me, "slug", device_id_slug());
  cJSON_AddBoolToObject(me, "self", true);
  cJSON_AddItemToArray(arr, me);

  mdns_result_t *results = NULL;
  // 1500 ms is a healthy window for same-LAN PTR; max 8 peers caps memory.
  esp_err_t qr = mdns_query_ptr("_wakelight", "_tcp", 1500, 8, &results);
  if (qr == ESP_OK) {
    for (mdns_result_t *r = results; r; r = r->next) {
      const char *slug = txt_lookup(r, "slug");
      const char *name = txt_lookup(r, "name");
      // Skip self (TXT slug matches ours) and any responder missing a slug.
      if (!slug || !slug[0]) continue;
      if (strcmp(slug, device_id_slug()) == 0) continue;
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", name && name[0] ? name : slug);
      cJSON_AddStringToObject(p, "slug", slug);
      cJSON_AddBoolToObject(p, "self", false);
      cJSON_AddItemToArray(arr, p);
    }
    mdns_query_results_free(results);
  } else {
    ESP_LOGW(TAG, "peers query failed: %d", qr);
  }

  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!out) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/json");
  esp_err_t r = httpd_resp_send(req, out, strlen(out));
  free(out);
  return r;
}

static esp_err_t device_get_h(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "name", device_id_name());
  cJSON_AddStringToObject(root, "slug", device_id_slug());
  cJSON_AddStringToObject(root, "hostname", device_id_chosen_hostname());
  cJSON_AddStringToObject(root, "default_hostname", device_id_default_hostname());
  cJSON_AddBoolToObject(root, "holds_wakelight", device_id_holds_wakelight());
  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!out) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/json");
  esp_err_t r = httpd_resp_send(req, out, strlen(out));
  free(out);
  return r;
}

static esp_err_t device_put_h(httpd_req_t *req) {
  if (req->content_len > 256) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too big");
  }
  char body[257];
  int total = 0;
  while (total < req->content_len) {
    int r = httpd_req_recv(req, body + total, req->content_len - total);
    if (r <= 0) return httpd_resp_send_500(req);
    total += r;
  }
  body[total] = 0;
  cJSON *root = cJSON_Parse(body);
  if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
  cJSON *name = cJSON_GetObjectItem(root, "name");
  if (!cJSON_IsString(name)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
  }
  char prospective[33];
  device_id_slug_for(name->valuestring, prospective, sizeof(prospective));
  // Skip the probe if the slug is what we already publish — otherwise we'd
  // see our own A-record and reject a no-op rename.
  if (strcmp(prospective, device_id_chosen_hostname()) != 0 &&
      hostname_taken_by_other(prospective)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    const char *msg = "{\"error\":\"name_taken\"}";
    return httpd_resp_send(req, msg, strlen(msg));
  }
  bool ok = device_id_set_name(name->valuestring);
  cJSON_Delete(root);
  if (!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
  apply_mdns_identity();
  return device_get_h(req);
}

void http_ui_start(void) {
  start_mdns();

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 6144;
  cfg.close_fn = ws_close_fn;
  cfg.max_uri_handlers = sizeof(uris) / sizeof(uris[0]);
  httpd_handle_t srv = NULL;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(srv, &uris[i]);
  }
  ESP_LOGI(TAG, "http server started");
}
