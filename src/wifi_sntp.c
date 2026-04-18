#include "wifi_sntp.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include <string.h>
#include <time.h>

#include "wifi_secrets.h"

static const char *TAG = "wifi";
#define CONNECTED_BIT BIT0
#define GOT_IP_BIT BIT1

static EventGroupHandle_t g_events;
static bool g_time_valid = false;

static void on_got_time(struct timeval *tv) {
  (void)tv;
  g_time_valid = true;
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &lt);
  ESP_LOGI(TAG, "time synced: %s", buf);
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg; (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "disconnected, reconnecting");
    xEventGroupClearBits(g_events, CONNECTED_BIT | GOT_IP_BIT);
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
    xEventGroupSetBits(g_events, CONNECTED_BIT | GOT_IP_BIT);
  }
}

bool wifi_sntp_start(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }

  g_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL, NULL));

  wifi_config_t wc = { 0 };
  strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
  strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
  wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  // Required for DMX: power save adds ~100ms radio sleeps that corrupt UART
  // timing. Costs ~20mA; we're mains-powered so we don't care.
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  ESP_LOGI(TAG, "connecting to SSID \"%s\"", WIFI_SSID);
  EventBits_t bits = xEventGroupWaitBits(g_events, GOT_IP_BIT, pdFALSE, pdTRUE,
                                         pdMS_TO_TICKS(30000));
  if (!(bits & GOT_IP_BIT)) {
    ESP_LOGE(TAG, "wifi connect timed out");
    return false;
  }

  // London, auto DST.
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
  tzset();

  esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  sntp.sync_cb = on_got_time;
  esp_netif_sntp_init(&sntp);

  return true;
}

bool wifi_sntp_time_valid(void) { return g_time_valid; }
