// ============================================================================
//  btc_relay.cpp  -  see btc_relay.h
//
//  A core-0 task (isolated from the printer bridge on core 1):
//    - WiFi STA
//    - CoinGecko  : BTC + ETH + USDT (USD and BRL, 24h change)  -> ENOW_PRICES
//    - Open-Meteo : weather (current + 24h + min/max)           -> ENOW_WEATHER
//    - MQTT       : subscribe topics and forward messages       -> ENOW_MQTT
//    - NTP        : UTC epoch, stamped into every price packet
//  All of it broadcast over ESP-NOW to the ESP32-C3 display.
// ============================================================================
#include "config.h"

#ifndef BTC_RELAY_ENABLE
#define BTC_RELAY_ENABLE 0
#endif

#if BTC_RELAY_ENABLE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <lwip/dns.h>
#include <mbedtls/base64.h>
#include "btc_relay.h"
#include "enow_proto.h"

// ---- defaults (override in include/config.h) ----
#ifndef BTC_WIFI_SSID
#define BTC_WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef BTC_WIFI_PASS
#define BTC_WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif
#ifndef BTC_FETCH_MS
#define BTC_FETCH_MS 60000UL
#endif
#ifndef BTC_RETRY_MS
#define BTC_RETRY_MS 15000UL
#endif
#ifndef BTC_WEATHER_MS
#define BTC_WEATHER_MS 600000UL
#endif
#ifndef BTC_WEATHER_LAT
#define BTC_WEATHER_LAT "51.5074"
#endif
#ifndef BTC_WEATHER_LON
#define BTC_WEATHER_LON "-0.1278"
#endif
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
#define BTC_MQTT_USER "esp32ticker"
#endif
#ifndef BTC_MQTT_PASS
#define BTC_MQTT_PASS "YOUR_MQTT_PASSWORD"
#endif
#ifndef BTC_RELAY_DEBUG
#define BTC_RELAY_DEBUG 1
#endif

#if BTC_RELAY_DEBUG
#define RLOG(...) Serial.printf("[btc] " __VA_ARGS__)
#else
#define RLOG(...)
#endif

static const char *MQTT_TOPICS[] = {
    "meshtastic/rx",
    "esp32ticker/alert",
    "printer/status",
    "home/front_door/state",
};
static const int MQTT_TOPIC_COUNT = sizeof(MQTT_TOPICS) / sizeof(MQTT_TOPICS[0]);
static const char *MQTT_CLIENT_ID = "esp32-btc-relay";
static const char *MQTT_STATUS_TOPIC = "esp32ticker/relay/status";
static const char *PRINTER_STATUS_TOPIC = "printer/status";

// printer status: written by core 1 (loop), published by core 0.
static volatile bool g_prnDirty = false;
static char g_prnState[48] = "starting";

void btcRelayPublishPrinter(const char *state)
{
    if (!state)
        return;
    // only flag it if it changed
    if (strncmp(g_prnState, state, sizeof(g_prnState)) == 0)
        return;
    strncpy(g_prnState, state, sizeof(g_prnState) - 1);
    g_prnState[sizeof(g_prnState) - 1] = 0;
    g_prnDirty = true;
}

static const char *PRICE_URL =
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids=bitcoin,ethereum,tether&vs_currencies=usd,brl&include_24hr_change=true";
static const char *WEATHER_URL =
    "https://api.open-meteo.com/v1/forecast?latitude=" BTC_WEATHER_LAT "&longitude=" BTC_WEATHER_LON
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
    "&hourly=temperature_2m&daily=temperature_2m_max,temperature_2m_min"
    "&timezone=America%2FSao_Paulo&forecast_days=1";

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint32_t g_seq = 0;

static WiFiClient mqttNet;
static PubSubClient mqtt(mqttNet);

// force a public DNS (the homelab DHCP DNS sometimes fails to resolve
// external names). Leaves the IP/DHCP alone -- only swaps the DNS servers.
static void forceDns()
{
    ip_addr_t d;
    IP_ADDR4(&d, 1, 1, 1, 1);
    dns_setserver(0, &d);
    IP_ADDR4(&d, 8, 8, 8, 8);
    dns_setserver(1, &d);
}

// -------------------------- HTTP helper --------------------------------
static bool httpGetJsonOnce(const char *url, JsonDocument &doc)
{
    if (WiFi.status() != WL_CONNECTED)
        return false;
    WiFiClientSecure c;
    c.setInsecure();
    HTTPClient h;
    h.setConnectTimeout(8000);
    h.setTimeout(8000);
    if (!h.begin(c, url))
        return false;
    int code = h.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK)
    {
        // getString() de-chunks the response; deserializeJson(stream) sometimes
        // gets the raw chunked encoding and returns InvalidInput.
        String body = h.getString();
        DeserializationError e = deserializeJson(doc, body);
        if (!e)
            ok = true;
        else
            RLOG("JSON err: %s (%.60s)\n", e.c_str(), body.c_str());
    }
    else
        RLOG("HTTP %d at %.40s\n", code, url);
    h.end();
    return ok;
}

static bool httpGetJson(const char *url, JsonDocument &doc, size_t reserve)
{
    (void)reserve;
    for (int i = 1; i <= 3; i++)
    {
        if (httpGetJsonOnce(url, doc))
            return true;
        forceDns(); // the 1st failure is sometimes just DNS still coming up
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    return false;
}

// -------------------------- fetch price -------------------------------
static bool sendPrices()
{
    JsonDocument doc;
    if (!httpGetJson(PRICE_URL, doc, 1024))
        return false;

    enow_prices_t m = {};
    m.magic = ENOW_MAGIC;
    m.type = ENOW_PRICES;
    m.relay_ch = (uint8_t)WiFi.channel(); // the C3 follows this channel
    IPAddress lip = WiFi.localIP();        // the C3 shows this IP on the Printer screen
    for (int i = 0; i < 4; i++) m.relay_ip[i] = lip[i];
    m.seq = ++g_seq;
    time_t tnow = time(nullptr);
    m.epoch = (tnow > 1700000000) ? (uint32_t)tnow : 0; // 0 = NTP hasn't synced yet
    m.btc_usd = doc["bitcoin"]["usd"] | 0.0f;
    m.btc_brl = doc["bitcoin"]["brl"] | 0.0f;
    m.btc_chg = doc["bitcoin"]["usd_24h_change"] | 0.0f;
    m.eth_usd = doc["ethereum"]["usd"] | 0.0f;
    m.eth_brl = doc["ethereum"]["brl"] | 0.0f;
    m.eth_chg = doc["ethereum"]["usd_24h_change"] | 0.0f;
    m.usdt_usd = doc["tether"]["usd"] | 0.0f;
    m.usdt_brl = doc["tether"]["brl"] | 0.0f;
    m.usdt_chg = doc["tether"]["brl_24h_change"] | 0.0f; // exchange-rate change

    if (m.btc_usd <= 0)
        return false;
    esp_err_t e = esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
    RLOG("PRICES seq=%lu btc=%.0f eth=%.0f usdt_brl=%.3f send=%d\n",
         (unsigned long)m.seq, m.btc_usd, m.eth_usd, m.usdt_brl, (int)e);
    return e == ESP_OK;
}

// -------------------------- fetch weather -----------------------------
static bool sendWeather()
{
    JsonDocument doc;
    if (!httpGetJson(WEATHER_URL, doc, 8192))
        return false;

    enow_weather_t m = {};
    m.magic = ENOW_MAGIC;
    m.type = ENOW_WEATHER;
    m.seq = ++g_seq;
    m.temp = doc["current"]["temperature_2m"] | 0.0f;
    m.feels = doc["current"]["apparent_temperature"] | 0.0f;
    m.humidity = doc["current"]["relative_humidity_2m"] | 0.0f;
    m.wind = doc["current"]["wind_speed_10m"] | 0.0f;
    m.code = doc["current"]["weather_code"] | 0;
    m.tmax = doc["daily"]["temperature_2m_max"][0] | 0.0f;
    m.tmin = doc["daily"]["temperature_2m_min"][0] | 0.0f;

    uint8_t n = 0;
    for (JsonVariant v : doc["hourly"]["temperature_2m"].as<JsonArray>())
    {
        if (n >= 24)
            break;
        m.hourly[n++] = v.as<float>();
    }
    m.hourlyCount = n;

    esp_err_t e = esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
    RLOG("WEATHER seq=%lu %.1fC code=%d h=%d send=%d\n",
         (unsigned long)m.seq, m.temp, m.code, n, (int)e);
    return e == ESP_OK;
}

// -------------------------- MQTT --------------------------------------
static String b64decode(const char *in)
{
    if (!in || !*in)
        return String();
    size_t inLen = strlen(in), outLen = 0;
    // 1st call just to size the output
    mbedtls_base64_decode(nullptr, 0, &outLen, (const unsigned char *)in, inLen);
    if (outLen == 0 || outLen > 220)
        return String();
    unsigned char buf[224];
    if (mbedtls_base64_decode(buf, sizeof(buf), &outLen, (const unsigned char *)in, inLen) != 0)
        return String();
    buf[outLen] = 0;
    return String((char *)buf);
}

static String nodeTag(uint32_t id)
{
    if (id == 0xFFFFFFFFUL || id == 0)
        return "all";
    char b[12];
    snprintf(b, sizeof(b), "!%08x", (unsigned)id);
    return String(b);
}

// Returns "" if the message should be IGNORED (telemetry, admin, etc).
static String meshtasticToLines(const String &body)
{
    // clean format from the HA automation:  from|to|message
    int p1 = body.indexOf('|');
    int p2 = p1 >= 0 ? body.indexOf('|', p1 + 1) : -1;
    if (!body.startsWith("{") && p1 > 0 && p2 > p1)
        return "From: " + body.substring(0, p1) + "\nTo: " + body.substring(p1 + 1, p2) +
               "\n" + body.substring(p2 + 1);

    // raw HA event JSON: filter TEXT_MESSAGE_APP and decode the base64
    if (body.startsWith("{"))
    {
        JsonDocument d;
        if (deserializeJson(d, body))
            return ""; // invalid JSON -> ignore
        JsonObject data = d["data"];
        const char *portnum = data["decoded"]["portnum"] | "";
        if (strcmp(portnum, "TEXT_MESSAGE_APP") != 0)
            return ""; // telemetry/admin/routing -> ignore
        String text = b64decode(data["decoded"]["payload"] | "");
        if (text.length() == 0)
            return "";
        uint32_t from = data["from"] | 0UL;
        uint32_t to = data["to"] | 0UL;
        return "From: " + nodeTag(from) + "\nTo: " + nodeTag(to) + "\n" + text;
    }

    return body; // plain text on another topic
}

static uint8_t catForTopic(const char *topic)
{
    if (strcmp(topic, "meshtastic/rx") == 0)
        return MQ_MESH;
    if (strcmp(topic, "printer/status") == 0)
        return MQ_PRINT;
    if (strcmp(topic, "esp32ticker/alert") == 0 || strncmp(topic, "home/", 5) == 0)
        return MQ_ALERT;
    return MQ_OTHER;
}

static void onMqtt(char *topic, byte *payload, unsigned int len)
{
    String body((const char *)payload, len);
    String out;
    uint8_t cat = catForTopic(topic);

    if (cat == MQ_MESH)
    {
        out = meshtasticToLines(body);
        if (out.length() == 0)
            return; // ignored (not a text message)
    }
    else if (cat == MQ_PRINT && body.startsWith("{"))
    {
        // {"state":"ready",...} -> "ready"
        int i = body.indexOf("\"state\"");
        int q1 = i >= 0 ? body.indexOf('"', i + 7) : -1;
        int q2 = q1 >= 0 ? body.indexOf('"', q1 + 1) : -1;
        out = (q1 >= 0 && q2 > q1) ? body.substring(q1 + 1, q2) : body;
    }
    else
        out = body;

    enow_mqtt_t m = {};
    m.magic = ENOW_MAGIC;
    m.type = ENOW_MQTT;
    m.cat = cat;
    m.seq = ++g_seq;
    strncpy(m.topic, topic, sizeof(m.topic) - 1);
    unsigned int n = out.length() < sizeof(m.payload) - 1 ? out.length() : sizeof(m.payload) - 1;
    memcpy(m.payload, out.c_str(), n);
    m.payload[n] = 0;
    esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
    RLOG("MQTT [%s] %s\n", m.topic, m.payload);
}

static void mqttReconnect()
{
    static uint32_t last = 0;
    static int lastRc = 99;
    if (mqtt.connected() || millis() - last < 20000) // spaced-out retry
        return;
    last = millis();
    if (mqtt.connect(MQTT_CLIENT_ID, BTC_MQTT_USER, BTC_MQTT_PASS,
                     MQTT_STATUS_TOPIC, 1, true, "offline"))
    {
        mqtt.publish(MQTT_STATUS_TOPIC, "online", true);
        for (int i = 0; i < MQTT_TOPIC_COUNT; i++)
            mqtt.subscribe(MQTT_TOPICS[i]);
        RLOG("MQTT connected\n");
        lastRc = 0;
    }
    else
    {
        int rc = mqtt.state();
        if (rc != lastRc) // log only when it changes
            RLOG("MQTT not connected rc=%d (5=bad password; set BTC_MQTT_* in config.h)\n", rc);
        lastRc = rc;
    }
}

static void publishPrinterStatusIfDue()
{
    static uint32_t nextBeat = 0;
    if (!mqtt.connected())
        return;
    if (!g_prnDirty && (int32_t)(millis() - nextBeat) < 0)
        return;
    g_prnDirty = false;
    nextBeat = millis() + 60000; // republish periodically (keeps the "Xs ago" fresh)
    char j[96];
    snprintf(j, sizeof(j), "{\"state\":\"%s\",\"ts\":%ld}", g_prnState, (long)time(nullptr));
    mqtt.publish(PRINTER_STATUS_TOPIC, j, true); // retained
    RLOG("printer/status -> %s\n", g_prnState);
}

// -------------------------- task ------------------------------------
static void relayTask(void *)
{
    WiFi.persistent(false);
    WiFi.setSleep(false);
    // if web_print (or another module) already associated, don't restart the connection
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.mode(WIFI_STA);
        WiFi.begin(BTC_WIFI_SSID, BTC_WIFI_PASS);
        RLOG("WiFi \"%s\" ...\n", BTC_WIFI_SSID);
    }
    else
        RLOG("WiFi already associated (IP=%s)\n", WiFi.localIP().toString().c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000)
        vTaskDelay(pdMS_TO_TICKS(250));
    if (WiFi.status() == WL_CONNECTED)
    {
        forceDns();
        configTime(0, 0, "pool.ntp.org", "time.nist.gov", "a.st1.ntp.br"); // UTC epoch
        RLOG("WiFi OK IP=%s channel=%d dns=1.1.1.1 (NTP syncing)\n",
             WiFi.localIP().toString().c_str(), WiFi.channel());
    }
    else
        RLOG("WiFi not connected (retrying in bg)\n");

    if (esp_now_init() != ESP_OK)
    {
        RLOG("esp_now_init FAILED\n");
        vTaskDelete(NULL);
        return;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = 0; // current STA channel
    peer.encrypt = false;
    esp_now_add_peer(&peer);

#if BTC_MQTT_ENABLE
    mqtt.setBufferSize(512);
    mqtt.setServer(BTC_MQTT_HOST, BTC_MQTT_PORT);
    mqtt.setCallback(onMqtt);
#endif

    uint32_t nextPrice = 0, nextWx = 0;
    for (;;)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            WiFi.reconnect();
            vTaskDelay(pdMS_TO_TICKS(3000));
            if (WiFi.status() == WL_CONNECTED)
                forceDns();
            continue;
        }

#if BTC_MQTT_ENABLE
        if (!mqtt.connected())
            mqttReconnect();
        mqtt.loop();
        publishPrinterStatusIfDue();
#endif

        if ((int32_t)(millis() - nextPrice) >= 0)
        {
            bool ok = sendPrices();
            nextPrice = millis() + (ok ? BTC_FETCH_MS : BTC_RETRY_MS);
        }
        if ((int32_t)(millis() - nextWx) >= 0)
        {
            bool ok = sendWeather();
            nextWx = millis() + (ok ? BTC_WEATHER_MS : BTC_RETRY_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void btcRelayBegin()
{
    xTaskCreatePinnedToCore(relayTask, "btc_relay", 16384, nullptr, 1, nullptr, 0);
}

#else // BTC_RELAY_ENABLE == 0
void btcRelayBegin() {}
#endif
