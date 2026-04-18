#include "dmx_out.h"
#include "http_ui.h"
#include "override.h"
#include "schedule.h"
#include "wifi_sntp.h"

#define OVERRIDE_ON_BRIGHTNESS 100
#define OVERRIDE_ON_CCT 4000

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <time.h>

static const char *TAG = "wakelight";

static void ramp_task(void *arg) {
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(1000);
  TickType_t tick = xTaskGetTickCount();
  while (1) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    uint16_t mod = lt.tm_hour * 60 + lt.tm_min;

    override_mode_t ov = override_get();
    uint8_t out_b = 0;
    uint16_t out_k = 2700;
    if (ov == OVERRIDE_ON) {
      out_b = OVERRIDE_ON_BRIGHTNESS;
      out_k = OVERRIDE_ON_CCT;
    } else if (ov == OVERRIDE_OFF) {
      out_b = 0;
      out_k = 2700;
    } else {
      schedule_t s;
      schedule_get(&s);
      uint8_t b = 0;
      uint16_t k = 2700;
      bool active = schedule_eval(&s, mod, &b, &k);
      out_b = active ? b : 0;
      out_k = active ? k : 2700;
    }
    dmx_out_set(out_b, out_k);
    vTaskDelayUntil(&tick, period);
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "wakelight booting");

  dmx_out_start();
  dmx_out_set(0, 2700);

  schedule_t s;
  schedule_load(&s);

  if (!wifi_sntp_start()) {
    ESP_LOGE(TAG, "wifi failed; continuing without network");
  } else {
    http_ui_start();
  }

  xTaskCreate(ramp_task, "ramp", 4096, NULL, 4, NULL);
}
