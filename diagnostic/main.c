// DMX bus diagnostic firmware.
// Use when bringing up a new fixture or a flaky RS485 link.
// Build: `pio run -e diagnostic -t upload`
//
// Streams a 2s ON / 2s OFF blink on a Neewer PL60C at DMX address 1
// (adjust FIXTURE_ADDR for other fixtures), and logs per-second TX
// counters so you can tell whether the ESP side is sending cleanly:
//
//   tx: sent=34 zero=0 wait_ok=34 wait_timeout=0 enabled=1 last_n=513
//
// Expected healthy stream at 33 Hz: sent=~34, zero=0, wait_timeout=0.
// If sent=34 and wait_timeout=0 but the fixture is intermittent,
// the problem is physical: A/B polarity, termination, or grounding.

#include "esp_dmx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TX_PIN 17
#define RX_PIN 16
#define EN_PIN 4

#define FIXTURE_ADDR 1

static const char *TAG = "diagnostic";
static const dmx_port_t kDmxPort = DMX_NUM_1;
static uint8_t g_dmx[DMX_PACKET_SIZE];

static void apply(uint8_t brightness) {
  uint8_t *f = &g_dmx[FIXTURE_ADDR];
  f[0] = 0;
  f[1] = brightness;
  f[2] = 255;
  f[3] = 128;
  dmx_write(kDmxPort, g_dmx, DMX_PACKET_SIZE);
}

void app_main(void) {
  ESP_LOGI(TAG, "=== DMX diagnostic @ addr %d ===", FIXTURE_ADDR);

  dmx_config_t config = DMX_CONFIG_DEFAULT;
  if (!dmx_driver_install(kDmxPort, &config, NULL, 0)) {
    ESP_LOGE(TAG, "driver install failed");
    return;
  }
  dmx_set_pin(kDmxPort, TX_PIN, RX_PIN, EN_PIN);
  ESP_LOGI(TAG, "driver up, enabled=%d", dmx_driver_is_enabled(kDmxPort));

  const TickType_t period = pdMS_TO_TICKS(30);
  const int64_t blink_us = 2000000;
  int64_t last_toggle = esp_timer_get_time();
  int64_t last_stats = last_toggle;
  bool on = true;
  int cycle = 0;

  uint32_t sends_ok = 0, sends_zero = 0, waits_ok = 0, waits_timeout = 0;

  apply(255);
  ESP_LOGI(TAG, "#0 ON");

  TickType_t tick = xTaskGetTickCount();
  while (1) {
    int64_t now = esp_timer_get_time();

    if (now - last_toggle >= blink_us) {
      on = !on;
      apply(on ? 255 : 0);
      last_toggle += blink_us;
      if (on) cycle++;
      ESP_LOGI(TAG, "#%d %s", cycle, on ? "ON" : "OFF");
    }

    size_t n = dmx_send_num(kDmxPort, DMX_PACKET_SIZE);
    if (n > 0) sends_ok++; else sends_zero++;
    bool ok = dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
    if (ok) waits_ok++; else waits_timeout++;

    if (now - last_stats >= 1000000) {
      ESP_LOGI(TAG, "tx: sent=%" PRIu32 " zero=%" PRIu32
                    " wait_ok=%" PRIu32 " wait_timeout=%" PRIu32
                    " enabled=%d last_n=%u",
               sends_ok, sends_zero, waits_ok, waits_timeout,
               dmx_driver_is_enabled(kDmxPort), (unsigned)n);
      sends_ok = sends_zero = waits_ok = waits_timeout = 0;
      last_stats = now;
    }

    vTaskDelayUntil(&tick, period);
  }
}
