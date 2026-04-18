#include "dmx_out.h"

#include "esp_dmx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <string.h>

#define TX_PIN 17
#define RX_PIN 16
#define EN_PIN 4

// Neewer PL60C, DMX address 1, Mode 1 (CCT).
// Slots (0-indexed from FIXTURE_ADDR): [mode, brightness, CCT, G/M].
// Mode byte = 0 selects CCT sub-mode within Mode 1.
// CCT byte: 0 -> 2500K, 255 -> 10000K.
// G/M: 128 = neutral.
#define FIXTURE_ADDR 1

static const char *TAG = "dmx";
static const dmx_port_t kDmxPort = DMX_NUM_1;
static uint8_t g_frame[DMX_PACKET_SIZE];

// Packed state: low 8 bits brightness(0..255), next 8 bits cct byte (0..255),
// bit 16 = force-off. Updated atomically from any thread.
static _Atomic uint32_t g_state = 0;

static uint8_t pct_to_byte(uint8_t pct) {
  if (pct >= 100) return 255;
  return (uint16_t)pct * 255 / 100;
}

static uint8_t cct_to_byte(uint16_t cct_k) {
  if (cct_k <= 2500) return 0;
  if (cct_k >= 10000) return 255;
  return (uint16_t)(((uint32_t)(cct_k - 2500) * 255) / 7500);
}

static void sender_task(void *arg) {
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(30);
  TickType_t tick = xTaskGetTickCount();
  while (1) {
    uint32_t s = atomic_load(&g_state);
    uint8_t bright = s & 0xFF;
    uint8_t cct = (s >> 8) & 0xFF;
    bool force_off = (s >> 16) & 1;

    uint8_t *f = &g_frame[FIXTURE_ADDR];
    f[0] = 0;                           // mode select -> CCT
    f[1] = force_off ? 0 : bright;      // brightness
    f[2] = cct;                         // colour temp
    f[3] = 128;                         // G/M neutral

    dmx_write(kDmxPort, g_frame, DMX_PACKET_SIZE);
    dmx_send_num(kDmxPort, DMX_PACKET_SIZE);
    dmx_wait_sent(kDmxPort, DMX_TIMEOUT_TICK);
    vTaskDelayUntil(&tick, period);
  }
}

void dmx_out_start(void) {
  dmx_config_t config = DMX_CONFIG_DEFAULT;
  if (!dmx_driver_install(kDmxPort, &config, NULL, 0)) {
    ESP_LOGE(TAG, "driver install failed");
    return;
  }
  dmx_set_pin(kDmxPort, TX_PIN, RX_PIN, EN_PIN);
  memset(g_frame, 0, sizeof(g_frame));
  xTaskCreatePinnedToCore(sender_task, "dmx_tx", 3072, NULL, 5, NULL, 1);
  ESP_LOGI(TAG, "started on UART%d", kDmxPort);
}

void dmx_out_set(uint8_t brightness_pct, uint16_t cct_k) {
  uint32_t s = pct_to_byte(brightness_pct) | ((uint32_t)cct_to_byte(cct_k) << 8);
  atomic_store(&g_state, s);
}

void dmx_out_off(void) {
  uint32_t s = atomic_load(&g_state);
  atomic_store(&g_state, s | (1u << 16));
}
