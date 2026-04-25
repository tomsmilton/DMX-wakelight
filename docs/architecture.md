## Firmware architecture

```mermaid
flowchart TB
  subgraph UI["User Interface (Browser)"]
    IDX["Schedule Page<br/>index.html"]
    LIV["Live Page<br/>live.html"]
  end

  subgraph FW["ESP32 Firmware"]
    direction TB

    subgraph NET["wifi_sntp.c"]
      WIFI["STA join + SNTP<br/>London TZ, WIFI_PS_NONE"]
    end

    subgraph HTTP["http_ui.c"]
      HSRV["HTTP server + mDNS<br/>GET / , GET /live (gzipped HTML)<br/>GET/PUT /api/schedule<br/>GET /api/status<br/>POST /api/override<br/>POST /api/dismiss"]
      WS["WebSocket /ws/live<br/>recv {b, k, gm}"]
    end

    subgraph STATE["State"]
      SCHED["schedule.c<br/>schedule_t { waypoint_t[10] }<br/>mutex-guarded g_cur"]
      OVR["override.c<br/>override_mode_t + manual vals<br/>packed in atomic uint32 (RAM only)"]
      DISM["dismiss.c<br/>g_dismissed_ord (atomic int)<br/>YYYY*512 + MM*32 + DD"]
    end

    RAMP["main.c — ramp_task<br/>1 Hz, prio 4<br/>reads override → dismiss → schedule<br/>resolves (byte, cct_k, gm)"]
    DMXC["dmx_out.c<br/>atomic packed g_state<br/>cct_to_byte(2500..10000 → 0..255)"]
    SEND["sender_task<br/>33 Hz (30 ms), prio 10, core 1<br/>writes slots [1..4] of DMX frame"]
  end

  NVS[("NVS<br/>ns=wakelight<br/>keys: schedule, dism_ord")]

  subgraph HW["Physical"]
    RS485["RS-485 transceiver<br/>TX=GPIO17 · RX=GPIO16 · DE/RE=GPIO4"]
    LAMP["Neewer PL60C @ DMX addr 1<br/>Mode 1 CCT<br/>slots: mode · bright · CCT · G/M"]
  end

  IDX -- "REST JSON" --> HSRV
  LIV -- "GET /live" --> HSRV
  LIV -- "JSON over WS" --> WS

  HSRV -- "schedule_get / schedule_save" --> SCHED
  HSRV -- "override_set" --> OVR
  HSRV -- "dismiss_for_today" --> DISM
  HSRV -- "override_get + schedule_eval<br/>(mirrors ramp for /api/status)" --> RAMP

  WS -- "override_set_manual()" --> OVR
  WS -- "dmx_out_set() (immediate)" --> DMXC

  SCHED <--> NVS
  DISM  <--> NVS

  WIFI --> HSRV
  WIFI --> WS

  SCHED -. "schedule_get" .-> RAMP
  OVR   -. "override_get / override_get_manual" .-> RAMP
  DISM  -. "dismiss_is_active" .-> RAMP
  SCHED -. "schedule_save drops dismiss<br/>if first waypoint is future" .-> DISM

  RAMP -- "dmx_out_set(byte, cct_k, gm)" --> DMXC
  DMXC --> SEND
  SEND -- "dmx_write + dmx_send_num<br/>via esp_dmx on UART1" --> RS485
  RS485 -- "DMX-512" --> LAMP
```

### Notes grounded in the code

- **Single decision point.** [`ramp_task`](../src/main.c) runs at 1 Hz and is the only
  path that translates override + dismiss + schedule into a DMX set point
  under `AUTO`. `MANUAL` and `ON`/`OFF` short-circuit earlier in the same
  task.
- **WS bypass for latency.** [`live_ws_h`](../src/http_ui.c) calls
  `dmx_out_set()` directly after `override_set_manual()` so slider moves
  don’t wait for the 1 Hz tick. The next `ramp_task` tick then repaints
  the same values from the atomic override state.
- **Two atomic stores, no queues.** `override.c` packs mode + manual
  (brightness_pct, cct_k, gm_byte) into one `_Atomic uint32_t`; `dmx_out.c`
  packs (bright, cct_byte, gm, force_off) into another. Neither uses a
  lock.
- **Schedule uses a mutex.** `schedule_t` is ~120 bytes so it’s copied
  under `g_lock` in [`schedule_get`](../src/schedule.c) / `schedule_save`.
- **NVS is only for schedule + dismiss.** Override is deliberately
  RAM-only — boot returns to `AUTO`.
- **Dismiss auto-clears** inside `schedule_save` when the new schedule’s
  first waypoint is later than the current minute-of-day (treats
  "saved a forward schedule" as intent to fire).
- **DMX pacing.** `sender_task` is pinned to core 1 at priority 10, above
  `ramp_task` (prio 4), so WiFi work on core 0 can’t starve the 30 ms
  frame cadence. `CONFIG_DMX_ISR_IN_IRAM=1` (see `platformio.ini`) keeps
  the ISR resident during flash-cache disables.
