# Wakelight Control API

HTTP + WebSocket control surface for the Wakelight ESP32 firmware. A
single Neewer PL60C CCT lamp on DMX address 1.

This is the full external API — there is nothing else. No auth, no
rate limiting, no SSL. It lives on a LAN and assumes the caller is
trusted.

## Addressing

- **Hostname**: `wakelight.local` (mDNS). If your resolver or network
  can't resolve mDNS, resolve once with
  `dns-sd -G v4 wakelight.local` (macOS) or `avahi-resolve -n
  wakelight.local` (Linux) and use the IP. mDNS can be flaky over
  some Wi-Fi stacks — cache the IP for a long-running agent and
  refresh on any connection error.
- **Port**: 80 (HTTP) and 80 (WebSocket, same port).
- **Example**: `http://wakelight.local/api/status` or
  `http://192.168.0.66/api/status`.

Most examples below show the hostname for readability; substitute an IP
if mDNS is unreliable.

## Time zone

All times exposed and accepted by the firmware are in the **device's
local wall-clock**, not UTC. The device is hardcoded to **Europe/London
with auto DST** (`GMT0BST,M3.5.0/1,M10.5.0`). If you're operating from
another timezone, do the conversion on your side before writing
`minute_of_day` values.

Time is sourced from SNTP (`pool.ntp.org`) at boot. Until SNTP syncs,
`state` is `"no_time"` and you should avoid writing schedules — the
device can't tell what day it is yet.

## Concepts

The lamp's effective output each moment is decided by four inputs,
combined in this priority order:

```
OVERRIDE  >  DISMISS  >  SCHEDULE
```

1. **Override** — one of `auto | on | off | manual`. Defaults to
   `auto` at boot (override is RAM-only). Anything other than `auto`
   short-circuits the schedule.
   - `on`: forces 255 DMX / 4000 K
   - `off`: forces 0 DMX
   - `manual`: driven by sliders over the WebSocket (see below).
     Cannot be set over REST — only a WS message can put the lamp into
     `manual`.
2. **Dismiss** — "done for today" flag, anchored to the local
   date. When active, AUTO mode treats the schedule as if disabled for
   the rest of the day. Persists to flash; auto-clears at local
   midnight.
3. **Schedule** — an ordered list of up to 10 waypoints, each
   `(minute_of_day, brightness %, CCT K)`. Between waypoints the
   firmware linearly interpolates in DMX-byte space for brightness,
   and in kelvin for CCT, once per second. Past the last waypoint,
   the lamp holds at those values until the next day or a new
   schedule. Persists to flash.

### Effective states (`status.state`)

| Value | Meaning |
|---|---|
| `no_time` | SNTP hasn't synced yet; do not write schedules. |
| `on` | Override is ON. |
| `off` | Override is OFF. |
| `manual` | WS slider session is active. |
| `dismissed` | AUTO mode, dismiss flag set for today. |
| `idle` | AUTO mode, before today's first waypoint (or schedule disabled). |
| `ramping` | AUTO mode, interpolating between two waypoints. |
| `holding` | AUTO mode, past the last waypoint, holding final values. |

## Endpoints

All bodies are JSON. No auth headers. Responses always have
`Content-Type: application/json` unless stated.

### `GET /api/status`

Current effective lamp state. Safe to poll at up to ~5 Hz. The UI
polls at 0.2 Hz.

**Response** (200):

```json
{
  "now":            "2026-04-19 18:42:04",
  "mod":            1122,
  "time_valid":     true,
  "active":         false,
  "brightness_pct": 0,
  "cct_k":          2700,
  "override":       "auto",
  "dismissed":      false,
  "gm":             0,
  "state":          "idle"
}
```

| Field | Type | Notes |
|---|---|---|
| `now` | string | Local wall-clock, `YYYY-MM-DD HH:MM:SS`. |
| `mod` | int | Minute-of-day in local time (`0..1439`). |
| `time_valid` | bool | False until SNTP syncs. |
| `active` | bool | True if lamp is emitting light (includes MANUAL). |
| `brightness_pct` | int | Effective brightness `0..100`. |
| `cct_k` | int | Effective colour temperature in kelvin. |
| `override` | string | One of `auto | on | off | manual`. |
| `dismissed` | bool | Today's ramp suppressed. |
| `gm` | int | Green/magenta tint, `-100..+100`. 0 = neutral. |
| `state` | string | See "Effective states" table above. |

### `GET /api/schedule`

Return the persisted schedule.

**Response** (200):

```json
{
  "enabled": true,
  "points": [
    {"m": 390, "b": 0,   "k": 2500},
    {"m": 410, "b": 30,  "k": 3000},
    {"m": 420, "b": 100, "k": 5000}
  ]
}
```

| Field | Type | Notes |
|---|---|---|
| `enabled` | bool | If false, schedule is dormant and AUTO behaves like `idle`. |
| `points[].m` | int | `minute_of_day` `0..1439` (local time). |
| `points[].b` | int | Brightness % `0..100`. |
| `points[].k` | int | CCT in kelvin `2500..10000`. |

### `PUT /api/schedule`

Replace the schedule. The firmware clamps every field to valid range,
sorts points by `m`, and persists to flash.

**Side effect**: if the new schedule is enabled, has at least one
point, and the first point's `m` is strictly greater than `now.mod`,
the dismiss flag is cleared (on the reasoning that saving a
forward-looking schedule is the user's signal they want it to fire).

**Request** (body <= 2048 bytes):

```json
{
  "enabled": true,
  "points": [
    {"m": 390, "b": 0, "k": 2500},
    {"m": 420, "b": 100, "k": 5000}
  ]
}
```

**Response** (200): the schedule as it was actually stored (post-clamp
and post-sort — the caller should treat this as the source of truth,
not the request body).

**Errors**:

| Code | Cause |
|---|---|
| 400 `body too big` | Body exceeded 2048 bytes. |
| 400 `invalid json` | Couldn't parse JSON, or `points` missing/not-array, or a `m`/`b`/`k` entry was non-numeric. |
| 500 | NVS write failed (rare; out-of-space or corrupt partition). |

**Notes**:
- Up to 10 points. Extras are silently truncated.
- Empty `points` array with `enabled: true` is legal but leaves AUTO
  in `idle` forever.
- `enabled: false` keeps the stored points but stops the ramp firing.

### `POST /api/override`

Set the override mode. Override is **RAM-only** — boot resets to
`auto`.

**Request**:

```json
{ "mode": "on" }
```

`mode` must be the literal string `"on"`, `"off"`, or `"auto"`. Any
other value (including `"manual"`, misspellings, or a missing/non-string
field) is silently coerced to `"auto"`. The response always reflects
the state actually applied — use that to detect coercion.

**Response** (200):

```json
{ "mode": "on" }
```

**Semantics**:
- `on` → 255 DMX byte (100 %), 4000 K, neutral tint.
- `off` → 0 DMX byte.
- `auto` → return to schedule + dismiss evaluation.
- Switching override does not touch the schedule or dismiss flag.
- There is no REST route to enter `manual`. Send a WS message instead.

### `POST /api/dismiss`

Mark today as dismissed. Terminal for the local date — there is **no
undo endpoint**. The flag auto-clears at local midnight, or when a
forward-looking schedule is written via `PUT /api/schedule`.

**Request**: no body required. If a body is sent it is ignored.

**Response** (200):

```json
{ "active": true }
```

## WebSocket

### `/ws/live`

Opens a persistent socket for live slider control. Dropping the socket
does **not** revert the lamp — once in `manual`, the lamp stays there
until an explicit `POST /api/override` with `"auto"` / `"on"` / `"off"`.

- **Subprotocol**: plain text JSON frames. No subprotocol header.
- **Max connections**: 4. Extra opens are accepted but older ones may
  get evicted on socket close.
- **Opening does nothing.** The mode does not flip to `manual` until the
  first valid slider frame arrives — so short-lived probing (e.g. a
  reconnect) won't steal from AUTO.

### Frame format (client → server)

Each frame is a single JSON object:

```json
{ "b": 45, "k": 3200, "gm": 0 }
```

| Field | Type | Range | Notes |
|---|---|---|---|
| `b` | int | 0..100 | Brightness %. Required. |
| `k` | int | 2500..10000 | CCT in kelvin. Required. |
| `gm` | int | -100..+100 | Green/magenta tint. Optional; defaults to 0 (neutral). Clamped. |

On each valid frame the firmware:
1. Sets override mode to `manual`.
2. Stores (b, k, gm) as the manual setpoint (atomic).
3. Pushes an updated DMX frame within ~30 ms.

Frames larger than 128 bytes are dropped. Non-text frames are
ignored. Malformed JSON is silently dropped.

### There is no server → client traffic

The WebSocket is write-only from the client's perspective. To observe
the resulting lamp state, poll `GET /api/status`.

### Suggested pacing

Client-side throttle of ~30 ms between frames is appropriate. The
reference UI coalesces slider movements on a 30 ms timer. Sending
much faster than that just queues up redundant frames.

## Recipes

### "Turn the lamp on to a fixed brightness/colour"

If you want a repeatable preset (e.g. "evening reading"), use the
WebSocket:

```python
import json, websocket
ws = websocket.create_connection("ws://wakelight.local/ws/live")
ws.send(json.dumps({"b": 40, "k": 2700, "gm": 0}))
ws.close()
```

Or if you just need max brightness, use the REST override:

```bash
curl -X POST http://wakelight.local/api/override \
  -H 'Content-Type: application/json' -d '{"mode":"on"}'
```

### "Turn the lamp off"

```bash
curl -X POST http://wakelight.local/api/override \
  -H 'Content-Type: application/json' -d '{"mode":"off"}'
```

This overrides the schedule. To return control to the schedule, send
`{"mode":"auto"}`.

### "Stop overriding, let the schedule run"

```bash
curl -X POST http://wakelight.local/api/override \
  -H 'Content-Type: application/json' -d '{"mode":"auto"}'
```

### "Set tomorrow's sunrise: 06:30 start, 07:00 at 100 % / 5000 K"

```bash
curl -X PUT http://wakelight.local/api/schedule \
  -H 'Content-Type: application/json' \
  -d '{
    "enabled": true,
    "points": [
      {"m": 390, "b": 0,   "k": 2500},
      {"m": 410, "b": 30,  "k": 3000},
      {"m": 420, "b": 100, "k": 5000}
    ]
  }'
```

(`m` = `hour * 60 + minute` local time.)

Three waypoints are typical — start, knee, finish — but any count up
to 10 is fine. The firmware linearly interpolates per second between
them.

### "Skip this morning's ramp"

```bash
curl -X POST http://wakelight.local/api/dismiss
```

Auto-clears at midnight.

### "Check whether the ramp is currently running"

```bash
curl -s http://wakelight.local/api/status | jq .state
```

Look for `ramping`, `holding`, or `idle`. If `no_time`, the clock
isn't synced yet — retry in a few seconds.

### "Smoothly fade from the current state to a new brightness over ~N seconds"

There is no server-side fade primitive for ad-hoc fades. Two options:

1. **Drive it from the client** — open the WebSocket, interpolate
   `b` and `k` locally at ~10 Hz, send each step. Closing the WS
   leaves the lamp at the last sent values.
2. **Use the schedule** — write a short 2-point schedule with `m` set
   to the current minute and the target minute. Simpler, but
   minute-granular start/end. Also: saving a forward-looking schedule
   clears the dismiss flag (mostly harmless for ad-hoc fades).

The WebSocket path is preferred for ad-hoc transitions.

## Limits and gotchas

- **No observable `manual` REST entrypoint.** To enter `manual`, you
  must open the WebSocket and send at least one valid frame.
- **Override is RAM-only.** A device reboot drops `on` / `off` /
  `manual` back to `auto`. Persisting intent is the caller's job.
- **Dismiss is one-shot per day and has no undo endpoint.** If you
  need to cancel a dismiss, writing any forward-looking schedule
  clears it.
- **Schedule writes are destructive.** The API is PUT (replace), not
  PATCH. To modify one point, read the current schedule, mutate the
  list, write it back. Note `PUT` echoes the clamped/sorted result,
  which may differ from the request.
- **Times are all local.** Hardcoded to Europe/London + DST. Adjust on
  the client if operating from elsewhere; there is no runtime way to
  change the TZ.
- **`state == "no_time"` is load-bearing.** During the 10-30 s window
  between boot and SNTP sync, the date is unknown — schedules still
  persist, but `state` stays `no_time` and AUTO produces no output.
  Wait for `time_valid: true` before writing a dismiss or a schedule
  that depends on "today".
- **Max 10 waypoints per schedule.** Extras are silently truncated.
- **PUT body cap 2048 bytes.** Overrun returns 400.
- **Override POST body cap 63 bytes.** Overrun is silently truncated;
  the JSON parser will usually then reject and the mode falls through
  to `auto`.
- **The device only drives one fixture**, on DMX address 1, mode 1
  (CCT). Multi-fixture control is out of scope.
- **The schedule graph persists.** The lamp will execute stored
  schedules on its own every morning regardless of network state — if
  the agent goes offline, yesterday's schedule still fires.

## Quick reference

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status` | Read current state. |
| GET | `/api/schedule` | Read stored schedule. |
| PUT | `/api/schedule` | Replace stored schedule. |
| POST | `/api/override` | Set override mode (`auto`/`on`/`off`). |
| POST | `/api/dismiss` | Mark today as dismissed. |
| WS | `/ws/live` | Drive `manual` mode in real time. |

| Literal | Value |
|---|---|
| `minute_of_day` range | `0..1439` |
| Brightness range | `0..100` % |
| CCT range | `2500..10000` K |
| GM range | `-100..+100` |
| Max schedule points | 10 |
| Max PUT body | 2048 bytes |
| Max WS frame | 128 bytes |
| Override POST body cap | 63 bytes |
