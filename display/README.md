# display — ESP32-C3 + ST7735 1.44"

The dashboard board. It runs **no WiFi** (see
[`../docs/esp32-c3-wifi-problem.md`](../docs/esp32-c3-wifi-problem.md)) — it
only listens for ESP-NOW packets from the relay.

## Screens

`Prev` / `Next` buttons (K1 = GPIO8, K2 = GPIO10, `INPUT_PULLUP`, active low)
cycle:

| # | Screen | Content |
|---|---|---|
| 1 | BTC/USD | big price with tween, 24h %, 40-pt sparkline, scrolling ticker |
| 2 | ETH/USD | same |
| 3 | USD/BRL | the FX rate via USDT, 2 decimals |
| 4 | Weather | temp, "feels like", code-drawn icon, 24h sparkline |
| 5 | Clock | NTP time relayed from the ESP32, blinking colon, date, minute bar |
| 6 | Meshtastic | rolling log of `meshtastic/rx` text messages |
| 7 | Alerts | rolling log of `esp32ticker/alerta`, `home/*` |
| 8 | Printer | rolling log of `impressora/status` |
| 9 | Info | link state, channel, packet counts, heap, uptime |

New Meshtastic / Alert messages also flash a banner over the current screen.

## Hardware

Spotpear "ESP32-C3 1.44 inch LCD" style module (ST7735S, 128×128, green tab).
Pin map is fixed on the board; it's passed in `platformio.ini`:

| ST7735 | GPIO |
|---|---|
| SCLK | 3 |
| MOSI | 4 |
| CS | 2 |
| DC | 0 |
| RST | 5 |
| backlight | tied to VCC (no control) |

## Notes

- **Library:** `Adafruit_ST7735` + `Adafruit_GFX`, rendering into a
  `GFXcanvas16` (32 KB) blitted in one shot. TFT_eSPI 2.5.x hangs in
  `tft.init()` on Arduino-ESP32 core 3.x for the C3, hence Adafruit.
- **Toolchain:** `espressif32@6.13.0` (Arduino-ESP32 2.0.17). `platform_packages`
  pins `tool-esptoolpy@4.5.1` — the newer 4.11 needs a python `intelhex`
  module PlatformIO doesn't ship.
- **Serial:** `pio device monitor` (miniterm) fails on this board's USB CDC
  ("termios ... not supported"); read the port with a small pyserial script.
- **Clock UTC offset:** `TZ_OFFSET_S` near the top of `src/main.cpp`
  (default −3 h). No DST handling.
- **Channel:** on boot the C3 sweeps `{6, 10, 1, 11}` until the first price
  packet, then follows `relay_ch` from every packet.

## Optional: a bitmap on the Clock / boot splash

`tools/img2sprite.py` converts a PNG **you have the rights to** into a C
header (`src/snoopy.h` / `src/snoopyMini.h`); the firmware auto-includes it
if present (`__has_include`) and falls back to a small code-drawn graphic
otherwise. These generated headers are `.gitignore`d.

```bash
python3 tools/img2sprite.py my_image.png snoopy 58
```

## Build

```bash
pio run -t upload
```

No configuration needed — this firmware has no secrets.
