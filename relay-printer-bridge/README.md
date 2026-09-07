# relay-printer-bridge — ESP32 WROOM-32

One ESP32 doing two unrelated jobs on two cores:

- **core 0** — the ESP-NOW relay: WiFi, HTTPS to CoinGecko + Open-Meteo,
  an MQTT client, NTP, and `esp_now_send()` broadcasts to the ESP32-C3
  display. This is why the project exists — the C3 can't do WiFi.
- **core 1** — a serial (USB) → parallel (Centronics / ESC-P) bridge for an
  **Epson LX-810L**, plus a web page, a REST endpoint and a raw TCP :9100
  network-printer endpoint.

The two never share a core, so TLS never jitters the parallel-port timing.

## Setup

Edit [`include/config.h`](include/config.h):

```c
#define BTC_WIFI_SSID "YOUR_WIFI_SSID"
#define BTC_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define BTC_WEATHER_LAT "51.5074"     // geocode your city
#define BTC_WEATHER_LON "-0.1278"
#define BTC_MQTT_HOST "192.168.1.10"  // your broker
#define BTC_MQTT_USER "mqttuser"
#define BTC_MQTT_PASS "YOUR_MQTT_PASSWORD"
```

Turn parts off if you don't need them:
`-D BTC_RELAY_ENABLE=0` · `-D BTC_MQTT_ENABLE=0` · `-D WEBPRINT_ENABLE=0` ·
`-D RAWPRINT_ENABLE=0`.

```bash
pio run -t upload
```

If the printer bridge isn't wired yet, build `-e bench` (ignores the printer
status lines, adds logs).

## Printing

| Method | How |
|---|---|
| Web page | `http://dotmatrix.local/` — textarea + Print + live status |
| REST | `curl --data-binary @file.txt -H 'Content-Type: text/plain' http://dotmatrix.local/print` |
| Network printer | Add by IP, *HP Jetdirect – Socket*, port 9100. mDNS name `EPSON LX-810L`. |

For the network printer, pick a driver:

- **Generic Text Only** — plain text, no formatting. Keep `RAWPRINT_RAW`
  as you like (line endings are normalized either way for this driver).
- **9-pin ESC/P** (e.g. CUPS "Epson 9-Pin Series", `drv:///sample.drv/epson9.ppd`)
  — text + bitmap graphics rendered as ESC/P, which the LX-810L executes.
  Requires `-D RAWPRINT_RAW=1` (pure passthrough).

It is **not AirPrint** — an ESP32 can't render PDF/PWG-raster. The LX-810L
does the ESC/P interpretation.

## Printer status → MQTT

Published retained to `impressora/status` as
`{"estado":"...","ts":...}`, on change and on a 60 s heartbeat. States:
`pronta` (ready), `imprimindo` (printing), `sem papel` (out of paper),
`off-line ou desligada`, `erro na impressora (/ERROR)`. The relay also
subscribes to it and forwards the `estado` string to the display's Printer
screen.

## Hardware

ESP32 classic (WROOM-32) wired to a DB25 male connector, with level
shifting on the 5 V status lines (the ESP32 is not 5 V tolerant).
Pin map, dividers and the schematic are in [`docs/`](docs/)
(`wiring.md`, `bringup.md`, `protocol.md`, `schematic.pdf`).

Reflash gotcha: if the board is stuck echoing garbage and won't run the
app, it entered `boot:0x7 (DOWNLOAD_BOOT)` — flash again with a normal
`--after hard_reset`, not `--after no_reset`.
