#include "http_ui.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"
#include "override.h"
#include "schedule.h"

#include <string.h>
#include <time.h>

static const char *TAG = "http";

extern const char index_html_data[];
extern const size_t index_html_len;

static esp_err_t root_get(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, index_html_data, index_html_len);
}

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
  uint8_t b = 0;
  uint16_t k = 2700;
  bool active = schedule_eval(&s, mod, &b, &k);

  override_mode_t ov = override_get();

  cJSON *root = cJSON_CreateObject();
  char tbuf[32];
  strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &lt);
  cJSON_AddStringToObject(root, "now", tbuf);
  cJSON_AddNumberToObject(root, "mod", mod);
  cJSON_AddBoolToObject(root, "time_valid", now > 1700000000);
  cJSON_AddBoolToObject(root, "active", active);
  cJSON_AddNumberToObject(root, "brightness_pct", b);
  cJSON_AddNumberToObject(root, "cct_k", k);
  cJSON_AddStringToObject(root, "override", override_name(ov));

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

static const httpd_uri_t uris[] = {
  {.uri = "/",              .method = HTTP_GET,  .handler = root_get},
  {.uri = "/api/schedule",  .method = HTTP_GET,  .handler = schedule_get_h},
  {.uri = "/api/schedule",  .method = HTTP_PUT,  .handler = schedule_put_h},
  {.uri = "/api/status",    .method = HTTP_GET,  .handler = status_get_h},
  {.uri = "/api/override",  .method = HTTP_POST, .handler = override_post_h},
};

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
