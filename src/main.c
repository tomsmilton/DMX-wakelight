#include "dismiss.h"
#include "dmx_out.h"
#include "http_ui.h"
#include "override.h"
#include "schedule.h"
#include "wifi_sntp.h"

#define OVERRIDE_ON_BYTE 255  // full brightness
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
    uint8_t out_byte = 0;
    uint16_t out_k = 2700;
    uint8_t out_gm = 128;  // neutral unless MANUAL supplies something else
    if (ov == OVERRIDE_ON) {
      out_byte = OVERRIDE_ON_BYTE;
      out_k = OVERRIDE_ON_CCT;
    } else if (ov == OVERRIDE_OFF) {
      out_byte = 0;
      out_k = 2700;
    } else if (ov == OVERRIDE_MANUAL) {
      uint8_t mpct = 0;
      override_get_manual(&mpct, &out_k, &out_gm);
      out_byte = (mpct >= 100) ? 255 : (uint8_t)((uint32_t)mpct * 255 / 100);
    } else {
      // AUTO: follow schedule unless the user dismissed for today.
      if (dismiss_is_active()) {
        out_byte = 0;
        out_k = 2700;
      } else {
        schedule_t s;
        schedule_get(&s);
        uint8_t b = 0;
        uint16_t k = 2700;
        bool active = schedule_eval(&s, mod, &b, &k);
        out_byte = active ? b : 0;
        out_k = active ? k : 2700;
      }
    }
    dmx_out_set(out_byte, out_k, out_gm);
    vTaskDelayUntil(&tick, period);
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "wakelight booting");

  dmx_out_start();
  dmx_out_set(0, 2700, 128);

  schedule_t s;
  schedule_load(&s);
  dismiss_init();

  if (!wifi_sntp_start()) {
    ESP_LOGE(TAG, "wifi failed; continuing without network");
  } else {
    http_ui_start();
  }

  xTaskCreate(ramp_task, "ramp", 4096, NULL, 4, NULL);
}
