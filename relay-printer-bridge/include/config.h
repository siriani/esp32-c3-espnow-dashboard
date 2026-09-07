// ============================================================================
//  config.h  -  pin map + compile-time options for the relay / printer bridge
//               (ESP32 WROOM-32  <->  Epson LX-810L, plus the ESP-NOW relay)
//
//  The pin map avoids the ESP32 classic's problem pins and works on both
//  WROOM-32 and WROVER:
//    GPIO 6..11    : wired to the SPI flash            -- never use
//    GPIO 0,2,5,12,15 : strapping pins (boot level matters) -- outputs only
//    GPIO 16,17    : used by PSRAM on WROVER modules
//    GPIO 34..39   : INPUT-ONLY, no internal pull      -- ok for status vias
//
//  On an ESP32-S3 / C3, review EVERY number below.
// ============================================================================
#pragma once

// ===========================================================================
//  PIN MAP  ==  same GPIO<->DB25 wiring as Manawyrm/ESP32_VirtualPrinter.
//  Directions are from the side that DRIVES the printer:
//    /STROBE + D0..D7 + /INIT + /AUTOFEED + /SELECT-IN  = OUTPUT
//    BUSY + PE + SELECT + /ERROR + /ACK                 = INPUT
//  Direct 3.3 V, or ~1k in series on each line (except GND).
// ===========================================================================
//  DATA BUS  D0..D7   (outputs)
// ---------------------------------------------------------------------------
#define PIN_D0       4   // DB25 pin 2
#define PIN_D1      14   // DB25 pin 3
#define PIN_D2      27   // DB25 pin 4
#define PIN_D3      26   // DB25 pin 5
#define PIN_D4      25   // DB25 pin 6
#define PIN_D5      33   // DB25 pin 7
#define PIN_D6      32   // DB25 pin 8
#define PIN_D7      18   // DB25 pin 9

// ---------------------------------------------------------------------------
//  CONTROL LINES  (ESP32 -> printer). ~1k in series recommended.
// ---------------------------------------------------------------------------
#define PIN_STROBE  13   // DB25 pin 1   (/STROBE)     low pulse = "latch the byte"
#define PIN_INIT    21   // DB25 pin 16  (/INIT)       low pulse = printer reset
#define PIN_AUTOFEED 23  // DB25 pin 14  (/AUTOFEED)   held HIGH (no auto-LF)
#define PIN_SELIN   19   // DB25 pin 17  (/SELECT-IN)  held LOW (select the printer)

// ---------------------------------------------------------------------------
//  STATUS FROM THE PRINTER  (inputs). 5 V lines -> ~1k series or a divider;
//  the ESP32 is NOT 5 V tolerant.
// ---------------------------------------------------------------------------
#define PIN_BUSY    17   // DB25 pin 11  (BUSY)    HIGH = printer busy
#define PIN_PE      16   // DB25 pin 12  (PE)      HIGH = out of paper
#define PIN_ERROR   22   // DB25 pin 15  (/ERROR)  LOW  = fault
#define PIN_SELECT  35   // DB25 pin 13  (SELECT)  HIGH = printer on-line
#define PIN_ACK     15   // DB25 pin 10  (/ACK)    low pulse after a byte is taken

// on-board LED on most DevKits
#define PIN_LED      2

// ===========================================================================
//  OPTIONS  (override with -D in platformio.ini)
// ===========================================================================
#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif

// 1 = TEXT MODE : CR, LF and CRLF are all normalized to CRLF (terminal text)
// 0 = RAW MODE  : stream untouched (binary ESC/P, graphics, ...)
#ifndef PRN_TEXT_MODE
#define PRN_TEXT_MODE 1
#endif

// XON/XOFF software flow control with the USB host (don't drop bytes on files)
#ifndef PRN_XONXOFF
#define PRN_XONXOFF 1
#endif

// at boot: pulse /INIT and send "ESC @" (ESC/P logical reset)
#ifndef PRN_AUTO_RESET_ON_BOOT
#define PRN_AUTO_RESET_ON_BOOT 1
#endif

// max wait for BUSY to clear before treating the printer as stuck
#ifndef PRN_BUSY_TIMEOUT_MS
#define PRN_BUSY_TIMEOUT_MS 4000
#endif

// 1 = ignore SELECT / PE / /ERROR, wait only on BUSY.
// Useful on the bench before the status dividers are wired.
#ifndef PRN_IGNORE_STATUS
#define PRN_IGNORE_STATUS 0
#endif

// ring buffer between the byte sources and the printer. MUST be a power of 2.
#ifndef PRN_RING_SIZE
#define PRN_RING_SIZE 4096
#endif

// UART RX buffer size (bytes). Set before Serial.begin().
#ifndef PRN_RX_BUFFER
#define PRN_RX_BUFFER 4096
#endif

#if (PRN_RING_SIZE & (PRN_RING_SIZE - 1)) != 0
#error "PRN_RING_SIZE must be a power of 2"
#endif

// ===========================================================================
//  ESP-NOW RELAY  -  core-0 task: fetch prices/weather, run an MQTT client
//  and NTP, broadcast to the ESP32-C3 display. Independent of the printer
//  bridge.  Disable with -D BTC_RELAY_ENABLE=0.
// ===========================================================================
#ifndef BTC_RELAY_ENABLE
#define BTC_RELAY_ENABLE 1
#endif

// ---- your WiFi ----
#ifndef BTC_WIFI_SSID
#define BTC_WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef BTC_WIFI_PASS
#define BTC_WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

#ifndef BTC_FETCH_MS
#define BTC_FETCH_MS 60000UL       // prices: once/min (CoinGecko free rate limit)
#endif
#ifndef BTC_WEATHER_MS
#define BTC_WEATHER_MS 600000UL    // weather: once/10min
#endif

// ---- weather (Open-Meteo, free, no key) ----
// geocode your city at:
// https://geocoding-api.open-meteo.com/v1/search?name=YOUR_CITY
#ifndef BTC_WEATHER_LAT
#define BTC_WEATHER_LAT "51.5074"  // London
#endif
#ifndef BTC_WEATHER_LON
#define BTC_WEATHER_LON "-0.1278"
#endif

// ---- MQTT (alerts forwarded to the display) ----
// Create the user on your broker with a real password and a tight ACL
// before enabling this. Disable with -D BTC_MQTT_ENABLE=0.
#ifndef BTC_MQTT_ENABLE
#define BTC_MQTT_ENABLE 1
#endif
#ifndef BTC_MQTT_HOST
#define BTC_MQTT_HOST "192.168.1.10"
#endif
#ifndef BTC_MQTT_PORT
#define BTC_MQTT_PORT 1883
#endif
#ifndef BTC_MQTT_USER
#define BTC_MQTT_USER "mqttuser"
#endif
#ifndef BTC_MQTT_PASS
#define BTC_MQTT_PASS "YOUR_MQTT_PASSWORD"
#endif

#ifndef BTC_RELAY_DEBUG
#define BTC_RELAY_DEBUG 1
#endif

// ===========================================================================
//  WEB PRINT  -  HTTP server on the ESP32 (runs in loop(), core 1, never
//  races the Centronics driver).
//    GET  /         page with a <textarea> + "Print" button
//    POST /print    print the form field "texto" OR the raw body
//                   (Content-Type: text/plain)   -> REST endpoint
//    GET  /status   JSON: printer state + queue occupancy
//  Disable: -D WEBPRINT_ENABLE=0
// ===========================================================================
#ifndef WEBPRINT_ENABLE
#define WEBPRINT_ENABLE 1
#endif

// ---- network printer: raw TCP :9100 (JetDirect) + mDNS as "EPSON LX-810L" ----
// Add it by IP on macOS/Windows (HP Jetdirect - Socket) and print from any
// app. Disable: -D RAWPRINT_ENABLE=0
#ifndef RAWPRINT_ENABLE
#define RAWPRINT_ENABLE 1
#endif
#ifndef RAWPRINT_PORT
#define RAWPRINT_PORT 9100
#endif
#ifndef RAWPRINT_MODEL
#define RAWPRINT_MODEL "EPSON LX-810L"
#endif
// 1 = port 9100 passes bytes THROUGH unchanged (required for a 9-pin ESC/P
//     driver -- graphics/images break if CR/LF is normalized).
// 0 = normalize line endings (only for the "Generic Text Only" driver).
#ifndef RAWPRINT_RAW
#define RAWPRINT_RAW 1
#endif

// by default the web/raw servers reuse the relay's WiFi credentials
#ifndef WEBPRINT_WIFI_SSID
#define WEBPRINT_WIFI_SSID BTC_WIFI_SSID
#endif
#ifndef WEBPRINT_WIFI_PASS
#define WEBPRINT_WIFI_PASS BTC_WIFI_PASS
#endif
#ifndef WEBPRINT_PORT
#define WEBPRINT_PORT 80
#endif
// mDNS name:  http://<hostname>.local/   ("" disables mDNS)
#ifndef WEBPRINT_HOSTNAME
#define WEBPRINT_HOSTNAME "dotmatrix"
#endif
// cap on the body accepted per request (bytes). WebServer buffers the whole
// body in RAM before the handler runs -- keep it modest.
#ifndef WEBPRINT_MAX_BODY
#define WEBPRINT_MAX_BODY 16384
#endif
// max time pushing one job into the queue before giving up (stuck printer)
#ifndef WEBPRINT_FEED_TIMEOUT_MS
#define WEBPRINT_FEED_TIMEOUT_MS 20000UL
#endif
// how long to wait for WiFi at boot (then it keeps connecting in background)
#ifndef WEBPRINT_WIFI_TIMEOUT_MS
#define WEBPRINT_WIFI_TIMEOUT_MS 15000UL
#endif
// optional token (light guard on /print and /status): if != "", require
// ?token=... in the URL or the X-Auth-Token header.
#ifndef WEBPRINT_TOKEN
#define WEBPRINT_TOKEN ""
#endif
#ifndef WEBPRINT_DEBUG
#define WEBPRINT_DEBUG 1
#endif
