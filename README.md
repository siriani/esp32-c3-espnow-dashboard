# esp32-c3-espnow-dashboard

A homelab status display built around a problem: **the ESP32-C3 board it runs
on cannot reliably join WiFi**, so it doesn't. A second ESP32 with a working
radio does all the networking and feeds the display over **ESP-NOW**. That
same second ESP32 also bridges a 1980s **Epson LX-810L dot-matrix printer**
onto the network (web page + JetDirect, printable from `Cmd+P`).

Two ESP32s, one story:

```
                Internet
                   │  HTTPS
        ┌──────────┴───────────┐
        │  RELAY  (ESP32 WROOM) │   also: USB↔parallel printer bridge,
        │  WiFi · MQTT · NTP    │         web UI, raw TCP :9100
        └──────────┬───────────┘
                   │  ESP-NOW (broadcast, no pairing)
        ┌──────────┴───────────┐
        │  DISPLAY (ESP32-C3)  │   1.44" ST7735, 9 screens, buttons
        │  no WiFi at all      │
        └──────────────────────┘
```

| Folder | Board | What it does |
|---|---|---|
| [`display/`](display/) | ESP32-C3 + ST7735 1.44" | The dashboard: BTC/ETH/USD-BRL, weather, clock, Meshtastic, alerts, printer status. Receives everything over ESP-NOW. |
| [`relay-printer-bridge/`](relay-printer-bridge/) | ESP32 WROOM-32 | Fetches prices/weather, runs an MQTT client, NTP, and rebroadcasts over ESP-NOW. Also a full serial→Centronics printer bridge with a web UI and a network-printer (port 9100) endpoint. |
| [`shared/`](shared/) | — | `enow_proto.h`, the ESP-NOW packet layout. Both projects `-I ../shared` against this one copy. |

## The ESP32-C3 WiFi problem (and why ESP-NOW fixes it)

The display board is a cheap ESP32-C3 + ST7735 "mini TV" module. Its Wi-Fi
**associates with phone hotspots but not with real access points** — every
attempt ends in a deauth with `reason 2` (`AUTH_EXPIRE`), on two
different-brand home routers, with the correct password and a -24 dBm signal.

Everything reasonable was tried and none of it helped: TX power 2–19.5 dBm,
`WiFi.setMinSecurity()`, all-channel scan, fixed BSSID/channel, PMF on/off,
802.11 b/g vs b/g/n, disabling power save, disabling Bluetooth, erasing NVS,
Arduino-ESP32 core 2.0.x **and** 3.x, and a full router-side "make it C3
friendly" pass (fixed channel, 20 MHz, WPA2-AES only, no band steering).

Root cause is hardware: the board ships with an **incomplete antenna
matching network** (two components are `DNP`/`TBD` on the schematic). The
radio works, but the RF is marginal enough that the tight timing of the WPA2
4-way handshake fails on anything but a very forgiving AP. This is a
well-known family of ESP32-C3 "SuperMini" complaints.

**The fix is to not use WiFi on the C3.** ESP-NOW is a connectionless
layer-2 protocol: single action frames, robust modulation, no association,
no handshake. At in-house range it is rock solid on the same marginal radio.
So a second ESP32 (any board with a real antenna) does the networking and
sends the display 20–60 byte packets.

Full write-up: [`docs/esp32-c3-wifi-problem.md`](docs/esp32-c3-wifi-problem.md).

### Channel following

ESP-NOW peers must sit on the same WiFi channel. The relay is a station, so
its channel is whatever the router hands it — and consumer routers roam. The
relay therefore stamps its current channel into every price packet, and the
display retunes to match. On a cold boot the display sweeps a short
candidate list until the first price packet arrives, then locks and follows.

## What's on the display

`Prev` / `Next` buttons cycle 9 screens:

`BTC/USD` · `ETH/USD` · `USD/BRL` · `Weather` · `Clock` · `Meshtastic` ·
`Alerts` · `Printer` · `Info`

- Prices: big number with a smooth tween on update, 24h change, a 40-point
  sparkline, a scrolling footer ticker.
- Weather: current temp + "feels like", a code-drawn condition icon, a 24h
  temperature sparkline. Source: [Open-Meteo](https://open-meteo.com/) (free,
  no key).
- Clock: NTP time relayed from the ESP32, kept with `millis()` between
  packets. UTC offset set in the display firmware.
- Meshtastic / Alerts / Printer: three independent rolling logs of MQTT
  messages, split by topic on the relay. Any new mesh/alert also flashes a
  banner over whatever screen you're on. JSON payloads are unwrapped to a
  readable line on both ends.
- Info: link status, current channel, packet counts, heap, uptime.

## The printer bridge

The relay ESP32 is wired to the parallel (Centronics) port of an Epson
LX-810L and exposes three ways to print:

1. **Web page** — `http://dotmatrix.local/` : a `<textarea>`, a Print
   button, live status.
2. **REST** — `POST /print` with `Content-Type: text/plain`.
3. **Network printer** — raw TCP on port **9100** (JetDirect), advertised
   over mDNS as `EPSON LX-810L`. Add it on macOS/Windows by IP with the
   *HP Jetdirect – Socket* protocol and a "Generic Text Only" or 9-pin
   ESC/P driver, then print from any app. It is **not** AirPrint (no PDF
   renderer on an ESP32); the real LX-810L interprets the ESC/P.

Printer state (`ready` / `printing` / `out of paper` / `offline` / `error`)
is published to MQTT (`printer/status`, retained) and shown on the
display's Printer screen.

Hardware notes, level-shifting and the DB25 wiring are in
[`relay-printer-bridge/docs/`](relay-printer-bridge/docs/).

## Build

[PlatformIO](https://platformio.org/). Each folder is its own project.

```bash
# display (ESP32-C3)
cd display && pio run -t upload

# relay + printer bridge (ESP32 WROOM)
cd relay-printer-bridge && pio run -t upload
```

Before flashing the relay, copy your settings into
[`relay-printer-bridge/include/config.h`](relay-printer-bridge/include/config.h)
(WiFi, MQTT broker, weather coordinates). The display has no secrets to set.

See each folder's `README.md` for wiring, toolchain notes and gotchas.

## Notes on the code

The whole tree is in English: `docs/`, every `README.md`, `config.h`,
`enow_proto.h`, the `platformio.ini` files, all on-screen strings, and every
comment in the source. On-the-wire names match the docs — MQTT `printer/status`
with a `{"state":"..."}` payload, and the `/status` JSON keys `ready` /
`state` / `queue_free` / `queue_total` / `queue_empty`.

## License

MIT — see [`LICENSE`](LICENSE).

---
topics: `esp32` · `esp32-c3` · `esp-now` · `homelab` · `st7735` · `platformio`
· `arduino` · `mqtt` · `dot-matrix-printer` · `jetdirect` · `iot`
