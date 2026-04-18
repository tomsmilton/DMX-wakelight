#include "esp_dmx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TX_PIN 17
#define RX_PIN 16
#define EN_PIN 4

// Neewer PL60C @ DMX address 1, CCT mode.
// Slots (0-indexed from FIXTURE_ADDR): [mode, brightness, CCT, G/M].
// Mode = 0 selects CCT; CCT byte: 0 = 2500K warm, 255 = 10000K cool.
#define FIXTURE_ADDR 1

static const char *TAG = "wakelight";
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
  ESP_LOGI(TAG, "wakelight blink test, addr %d", FIXTURE_ADDR);

  dmx_config_t config = DMX_CONFIG_DEFAULT;
  if (!dmx_driver_install(kDmxPort, &config, NULL, 0)) {
    ESP_LOGE(TAG, "driver install failed");
    return;
  }
  dmx_set_pin(kDmxPort, TX_PIN, RX_PIN, EN_PIN);

  const TickType_t period = pdMS_TO_TICKS(30);
  const int64_t blink_us = 2000000;
  int64_t last_toggle = esp_timer_get_time();
  bool on = true;
  int cycle = 0;

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
    dmx_send_num(kDmxPort, DMX_PACKET_SIZE);
    dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
    vTaskDelayUntil(&tick, period);
  }
}
