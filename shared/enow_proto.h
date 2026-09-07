// ============================================================================
//  ESP-NOW wire protocol between the relay (ESP32) and the display (ESP32-C3).
//
//  This file is the single source of truth. Both PlatformIO projects add
//  `-I ../shared` so they compile against THIS copy -- keep it that way.
//
//  ESP-NOW payloads are capped at 250 bytes. Every struct below fits.
//  Every packet starts with { magic = ENOW_MAGIC, type = EnowType }.
//
//  Note: the "brl" fields are the author's local currency (Brazilian Real),
//  pulled in the same CoinGecko call. Change `vs_currencies` in the relay and
//  rename if you want a different one -- it is just a second fiat column.
// ============================================================================
#pragma once
#include <stdint.h>

#define ENOW_MAGIC 0xB7

enum EnowType : uint8_t
{
    ENOW_PRICES = 1,
    ENOW_WEATHER = 2,
    ENOW_MQTT = 3,
};

// Which dedicated screen an MQTT message lands on (chosen by topic on the relay).
enum EnowMqttCat : uint8_t
{
    MQ_MESH = 0,  // meshtastic/rx
    MQ_ALERT = 1, // esp32ticker/alert, home/*, ...
    MQ_PRINT = 2, // printer/status
    MQ_OTHER = 3,
};

// --- prices (CoinGecko): BTC + ETH in USD/BRL, USDT as the USD/BRL rate ---
typedef struct __attribute__((packed))
{
    uint8_t magic;       // ENOW_MAGIC
    uint8_t type;        // ENOW_PRICES
    uint8_t relay_ch;    // relay's current WiFi channel -- the C3 follows it
    uint8_t relay_ip[4]; // relay's LAN IP (0.0.0.0 = no WiFi yet)
    uint8_t _pad;        // keep `seq` 4-byte aligned; DO NOT REMOVE
    uint32_t seq;
    uint32_t epoch;      // relay's UTC unix time (NTP); 0 = not synced yet
    float btc_usd, btc_brl, btc_chg;    // chg = usd_24h_change
    float eth_usd, eth_brl, eth_chg;    // chg = usd_24h_change
    float usdt_usd, usdt_brl, usdt_chg; // chg = brl_24h_change (the FX move)
} enow_prices_t;

// --- weather (Open-Meteo) ---
typedef struct __attribute__((packed))
{
    uint8_t magic; // ENOW_MAGIC
    uint8_t type;  // ENOW_WEATHER
    uint32_t seq;
    float temp, feels, humidity, wind, tmin, tmax;
    int16_t code;        // WMO weather code
    uint8_t hourlyCount; // valid slots in hourly[]
    float hourly[24];    // next-24h temperature, for the sparkline
} enow_weather_t;

// --- one forwarded MQTT message ---
typedef struct __attribute__((packed))
{
    uint8_t magic; // ENOW_MAGIC
    uint8_t type;  // ENOW_MQTT
    uint8_t cat;   // EnowMqttCat
    uint8_t _pad;
    uint32_t seq;
    char topic[48];
    char payload[160];
} enow_mqtt_t;
