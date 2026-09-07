# Architecture

```
                         ┌───────────────────────────────────────────┐
   api.coingecko.com ───►│                                           │
   api.open-meteo.com ──►│   RELAY  ·  ESP32 WROOM-32                 │
   MQTT broker ◄────────►│                                           │
   NTP pool ────────────►│   core 0 (FreeRTOS task):                 │
                         │     WiFi STA, HTTPS, MQTT, NTP,           │
                         │     ESP-NOW broadcast tx                  │
                         │   core 1 (Arduino loop):                 │
                         │     Centronics driver, web server,       │
                         │     TCP :9100, MQTT status producer      │
                         └───────────────┬───────────────┬──────────┘
                                         │ ESP-NOW       │ DB25 / level shift
                                         │ (2.4 GHz,     │
                                         │  broadcast)   ▼
                         ┌───────────────┴──────┐   ┌─────────────────┐
                         │  DISPLAY · ESP32-C3  │   │  Epson LX-810L   │
                         │  ST7735 1.44" 128²   │   │  9-pin, ESC/P    │
                         │  no WiFi; esp_now rx │   └─────────────────┘
                         │  + 2 buttons         │
                         └──────────────────────┘
```

## Why two boards

The C3 board's WiFi does not associate with normal access points — see
[`esp32-c3-wifi-problem.md`](esp32-c3-wifi-problem.md). ESP-NOW works on the
same radio, so networking is delegated to a second ESP32 that has a real
antenna.

## Relay (ESP32 WROOM-32)

Dual-core, so the jobs are pinned:

- **core 1** (`loop()`): the Centronics bit-bang driver, the HTTP server,
  the port-9100 listener, and the code that watches printer state and hands
  a status string to the relay task. Timing-sensitive parallel-port work
  never shares a core with TLS.
- **core 0** (`relayTask`): WiFi, `WiFiClientSecure` + `HTTPClient` for the
  two JSON APIs, `PubSubClient` for MQTT, `configTime()` for NTP, and all
  `esp_now_send()` calls. Publishes the queued printer status here too, so
  `PubSubClient` is only ever touched from one core.

Modules:

| File | Role |
|---|---|
| `main.cpp` | Centronics ring buffer, `loop()`, printer-status poll, wiring of the modules |
| `CentronicsPrinter.*` | per-byte parallel port driver: `/STROBE`, `BUSY`, `/ACK`, status lines, timeouts |
| `btc_relay.*` | the core-0 task: WiFi/HTTPS/MQTT/NTP + ESP-NOW tx; also `btcRelayPublishPrinter()` |
| `web_print.*` | HTTP server: `GET /` page, `POST /print`, `GET /status` |
| `raw_print.*` | raw TCP :9100 (JetDirect) + mDNS advertisement as a printer |
| `print_queue.h` | the small interface the web/raw producers use to enqueue bytes |

## Display (ESP32-C3)

Single file, `main.cpp`. Everything renders into a 128×128 16-bit
`GFXcanvas16` (Adafruit_GFX) and is pushed to the ST7735 in one blit — no
flicker, ~15 fps. `Adafruit_ST7735` is used instead of TFT_eSPI because
TFT_eSPI 2.5.x hangs in `tft.init()` on Arduino-ESP32 core 3.x for the C3.

- `onRecv()` (ESP-NOW callback): validates magic/type/length, updates the
  matching state (coin values + history + tween, weather arrays, MQTT ring
  per category), follows `relay_ch`, ingests `epoch` for the clock.
- `loop()`: buttons, channel sweep/lock, ~15 fps frame.
- No dynamic allocation after `setup()`; the canvas plus a handful of small
  ring buffers.

## ESP-NOW protocol

[`../shared/enow_proto.h`](../shared/enow_proto.h). Three packet types, all
`__attribute__((packed))`, all under the 250-byte ESP-NOW limit, all
prefixed with `{ magic, type }`. The receiver rejects anything whose length
isn't exactly `sizeof()` the expected struct.

| Type | Every | Carries |
|---|---|---|
| `ENOW_PRICES` | 60 s | BTC/ETH/USDT in USD+BRL + 24h change, `relay_ch`, `relay_ip`, `epoch` |
| `ENOW_WEATHER` | 10 min | current conditions + 24×hourly temp + min/max + WMO code |
| `ENOW_MQTT` | on message | `cat` (mesh/alert/print/other), topic, payload |

Keep the header byte-identical on both sides — the `-I ../shared` include
path makes that automatic; don't copy it back into either `src/`.
