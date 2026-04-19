#include "http_ui.h"

#include "cJSON.h"
#include "dismiss.h"
#include "dmx_out.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"
#include "override.h"
#include "schedule.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "http";

extern const unsigned char index_html_data[];
extern const size_t index_html_len;
extern const unsigned char live_html_data[];
extern const size_t live_html_len;

static esp_err_t send_gz_html(httpd_req_t *req, const unsigned char *buf, size_t len) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char *)buf, len);
}

static esp_err_t root_get(httpd_req_t *req) { return send_gz_html(req, index_html_data, index_html_len); }
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

  schedule_t s;
  schedule_get(&s);
  override_mode_t ov = override_get();
  bool dism = dismiss_is_active();

  // Mirror the ramp task's logic so the UI sees what actually goes to DMX.
  uint8_t byte = 0;
  uint16_t k = 2700;
  bool active = false;
  if (ov == OVERRIDE_ON) { byte = 255; k = 4000; active = true; }
  else if (ov == OVERRIDE_OFF) { byte = 0; k = 2700; active = false; }
  else if (ov == OVERRIDE_MANUAL) {
    uint8_t mp = 0; uint8_t gm = 128;
    override_get_manual(&mp, &k, &gm);
    byte = (mp >= 100) ? 255 : (uint8_t)((uint32_t)mp * 255 / 100);
    active = true;
  } else if (dism) {
    byte = 0; k = 2700; active = false;
  } else {
    active = schedule_eval(&s, mod, &byte, &k);
    if (!active) { byte = 0; k = 2700; }
  }
  uint8_t pct = (uint8_t)((uint32_t)byte * 100 / 255);

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
  if (g_ws_count == 0 && override_get() == OVERRIDE_MANUAL) {
    override_set(OVERRIDE_AUTO);
    ESP_LOGI(TAG, "ws drop -> auto");
  }
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
  // POST /api/dismiss  body: {"active":bool}. Missing/true = dismiss today,
  // false = undo.
  char body[64];
  int n = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
  int total = 0;
  while (total < n) {
    int r = httpd_req_recv(req, body + total, n - total);
    if (r <= 0) return httpd_resp_send_500(req);
    total += r;
  }
  body[total] = 0;
  bool active = true;
  if (total > 0) {
    cJSON *j = cJSON_Parse(body);
    if (j) {
      cJSON *a = cJSON_GetObjectItem(j, "active");
      if (cJSON_IsBool(a)) active = cJSON_IsTrue(a);
      cJSON_Delete(j);
    }
  }
  if (active) dismiss_for_today(); else dismiss_clear();
  ESP_LOGI(TAG, "dismiss -> %s", active ? "active" : "cleared");
  httpd_resp_set_type(req, "application/json");
  const char *r = active ? "{\"active\":true}" : "{\"active\":false}";
  return httpd_resp_send(req, r, strlen(r));
}

static const httpd_uri_t uris[] = {
  {.uri = "/",              .method = HTTP_GET,  .handler = root_get},
  {.uri = "/live",          .method = HTTP_GET,  .handler = live_get},
  {.uri = "/api/schedule",  .method = HTTP_GET,  .handler = schedule_get_h},
  {.uri = "/api/schedule",  .method = HTTP_PUT,  .handler = schedule_put_h},
  {.uri = "/api/status",    .method = HTTP_GET,  .handler = status_get_h},
  {.uri = "/api/override",  .method = HTTP_POST, .handler = override_post_h},
  {.uri = "/api/dismiss",   .method = HTTP_POST, .handler = dismiss_post_h},
  {.uri = "/ws/live",       .method = HTTP_GET,  .handler = live_ws_h, .is_websocket = true},
};

// Called by httpd when any socket closes (clean or dropped). We close the
// socket and, if it was one of our WS clients, fall back to AUTO.
static void ws_close_fn(httpd_handle_t hd, int fd) {
  (void)hd;
  ws_remove_fd(fd);
  close(fd);
}

static void start_mdns(void) {
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mdns_init failed: %d", err);
    return;
  }
  mdns_hostname_set("wakelight");
  mdns_instance_name_set("Wakelight");
  mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
  ESP_LOGI(TAG, "mdns: http://wakelight.local/");
}

void http_ui_start(void) {
  start_mdns();

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 6144;
  cfg.close_fn = ws_close_fn;
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
