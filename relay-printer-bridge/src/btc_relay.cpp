// ============================================================================
//  btc_relay.cpp  -  ver btc_relay.h
//
//  Task no core 0 (isolada da ponte da impressora, que fica no core 1):
//    - WiFi STA
//    - CoinGecko  : BTC + ETH + USDT (USD e BRL, variacao 24h)  -> ENOW_PRICES
//    - Open-Meteo : clima de London (atual + 24h + min/max)  -> ENOW_WEATHER
//    - MQTT       : assina topicos e encaminha as mensagens      -> ENOW_MQTT
//  Tudo por ESP-NOW broadcast pro display ESP32-C3 (Esp32C3_st7735).
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

// ---- defaults (sobrescrever em include/config.h) ----
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
    "esp32ticker/alerta",
    "impressora/status",
    "home/front_door/state",
};
static const int MQTT_TOPIC_COUNT = sizeof(MQTT_TOPICS) / sizeof(MQTT_TOPICS[0]);
static const char *MQTT_CLIENT_ID = "esp32-btc-relay";
static const char *MQTT_STATUS_TOPIC = "esp32ticker/relay/status";
static const char *PRINTER_STATUS_TOPIC = "impressora/status";

// status da impressora: escrito pelo core 1 (loop), publicado pelo core 0.
static volatile bool g_prnDirty = false;
static char g_prnEstado[48] = "iniciando";

void btcRelayPublishPrinter(const char *estado)
{
    if (!estado)
        return;
    // so marca se mudou
    if (strncmp(g_prnEstado, estado, sizeof(g_prnEstado)) == 0)
        return;
    strncpy(g_prnEstado, estado, sizeof(g_prnEstado) - 1);
    g_prnEstado[sizeof(g_prnEstado) - 1] = 0;
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

// forca DNS publico (o DNS do DHCP da rede homelab as vezes nao resolve
// nomes externos). Nao mexe no IP/DHCP -- so troca os servidores DNS.
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
        // getString() de-chunka a resposta; deserializeJson(stream) as vezes
        // recebe o encoding chunked cru e retorna InvalidInput.
        String body = h.getString();
        DeserializationError e = deserializeJson(doc, body);
        if (!e)
            ok = true;
        else
            RLOG("JSON err: %s (%.60s)\n", e.c_str(), body.c_str());
    }
    else
        RLOG("HTTP %d em %.40s\n", code, url);
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
        forceDns(); // 1a falha as vezes e DNS ainda subindo
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
    return false;
}

// -------------------------- fetch preco --------------------------------
static bool sendPrices()
{
    JsonDocument doc;
    if (!httpGetJson(PRICE_URL, doc, 1024))
        return false;

    enow_prices_t m = {};
    m.magic = ENOW_MAGIC;
    m.type = ENOW_PRICES;
    m.relay_ch = (uint8_t)WiFi.channel(); // o C3 segue este canal
    IPAddress lip = WiFi.localIP();        // o C3 mostra este IP na tela Impressao
    for (int i = 0; i < 4; i++) m.relay_ip[i] = lip[i];
    m.seq = ++g_seq;
    time_t tnow = time(nullptr);
    m.epoch = (tnow > 1700000000) ? (uint32_t)tnow : 0; // 0 = NTP ainda nao subiu
    m.btc_usd = doc["bitcoin"]["usd"] | 0.0f;
    m.btc_brl = doc["bitcoin"]["brl"] | 0.0f;
    m.btc_chg = doc["bitcoin"]["usd_24h_change"] | 0.0f;
    m.eth_usd = doc["ethereum"]["usd"] | 0.0f;
    m.eth_brl = doc["ethereum"]["brl"] | 0.0f;
    m.eth_chg = doc["ethereum"]["usd_24h_change"] | 0.0f;
    m.usdt_usd = doc["tether"]["usd"] | 0.0f;
    m.usdt_brl = doc["tether"]["brl"] | 0.0f;
    m.usdt_chg = doc["tether"]["brl_24h_change"] | 0.0f; // variacao do cambio

    if (m.btc_usd <= 0)
        return false;
    esp_err_t e = esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
    RLOG("PRICES seq=%lu btc=%.0f eth=%.0f usdt_brl=%.3f send=%d\n",
         (unsigned long)m.seq, m.btc_usd, m.eth_usd, m.usdt_brl, (int)e);
    return e == ESP_OK;
}

// -------------------------- fetch clima --------------------------------
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
    // 1a chamada so pra medir
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
        return "todos";
    char b[12];
    snprintf(b, sizeof(b), "!%08x", (unsigned)id);
    return String(b);
}

// Retorna "" se a mensagem deve ser IGNORADA (telemetria, admin, etc).
static String meshtasticToLines(const String &body)
{
    // formato limpo da automacao:  de|para|mensagem
    int p1 = body.indexOf('|');
    int p2 = p1 >= 0 ? body.indexOf('|', p1 + 1) : -1;
    if (!body.startsWith("{") && p1 > 0 && p2 > p1)
        return "De: " + body.substring(0, p1) + "\nPara: " + body.substring(p1 + 1, p2) +
               "\n" + body.substring(p2 + 1);

    // JSON cru do evento HA: filtra TEXT_MESSAGE_APP e decodifica o base64
    if (body.startsWith("{"))
    {
        JsonDocument d;
        if (deserializeJson(d, body))
            return ""; // JSON invalido -> ignora
        JsonObject data = d["data"];
        const char *portnum = data["decoded"]["portnum"] | "";
        if (strcmp(portnum, "TEXT_MESSAGE_APP") != 0)
            return ""; // telemetria/admin/routing -> ignora
        String text = b64decode(data["decoded"]["payload"] | "");
        if (text.length() == 0)
            return "";
        uint32_t from = data["from"] | 0UL;
        uint32_t to = data["to"] | 0UL;
        return "De: " + nodeTag(from) + "\nPara: " + nodeTag(to) + "\n" + text;
    }

    return body; // texto puro em outro topico
}

static uint8_t catForTopic(const char *topic)
{
    if (strcmp(topic, "meshtastic/rx") == 0)
        return MQ_MESH;
    if (strcmp(topic, "impressora/status") == 0)
        return MQ_PRINT;
    if (strcmp(topic, "esp32ticker/alerta") == 0 || strncmp(topic, "casa/", 5) == 0)
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
            return; // ignorado (nao era mensagem de texto)
    }
    else if (cat == MQ_PRINT && body.startsWith("{"))
    {
        // {"estado":"pronta",...} -> "pronta"
        int i = body.indexOf("\"estado\"");
        int q1 = i >= 0 ? body.indexOf('"', i + 8) : -1;
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
    if (mqtt.connected() || millis() - last < 20000) // retry espacado
        return;
    last = millis();
    if (mqtt.connect(MQTT_CLIENT_ID, BTC_MQTT_USER, BTC_MQTT_PASS,
                     MQTT_STATUS_TOPIC, 1, true, "offline"))
    {
        mqtt.publish(MQTT_STATUS_TOPIC, "online", true);
        for (int i = 0; i < MQTT_TOPIC_COUNT; i++)
            mqtt.subscribe(MQTT_TOPICS[i]);
        RLOG("MQTT conectado\n");
        lastRc = 0;
    }
    else
    {
        int rc = mqtt.state();
        if (rc != lastRc) // loga so quando muda
            RLOG("MQTT sem conexao rc=%d (5=senha; ajuste BTC_MQTT_* no config.h)\n", rc);
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
    nextBeat = millis() + 60000; // republica de tempos em tempos (mantem o "ha Xs")
    char j[96];
    snprintf(j, sizeof(j), "{\"estado\":\"%s\",\"ts\":%ld}", g_prnEstado, (long)time(nullptr));
    mqtt.publish(PRINTER_STATUS_TOPIC, j, true); // retido
    RLOG("impressora/status -> %s\n", g_prnEstado);
}

// -------------------------- task ------------------------------------
static void relayTask(void *)
{
    WiFi.persistent(false);
    WiFi.setSleep(false);
    // se o web_print (ou outro modulo) ja associou, nao reinicia a conexao
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.mode(WIFI_STA);
        WiFi.begin(BTC_WIFI_SSID, BTC_WIFI_PASS);
        RLOG("WiFi \"%s\" ...\n", BTC_WIFI_SSID);
    }
    else
        RLOG("WiFi ja associado (IP=%s)\n", WiFi.localIP().toString().c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000)
        vTaskDelay(pdMS_TO_TICKS(250));
    if (WiFi.status() == WL_CONNECTED)
    {
        forceDns();
        configTime(0, 0, "pool.ntp.org", "time.nist.gov", "a.st1.ntp.br"); // epoch UTC
        RLOG("WiFi OK IP=%s canal=%d dns=1.1.1.1 (NTP a sincronizar)\n",
             WiFi.localIP().toString().c_str(), WiFi.channel());
    }
    else
        RLOG("WiFi sem conexao (retenta em bg)\n");

    if (esp_now_init() != ESP_OK)
    {
        RLOG("esp_now_init FALHOU\n");
        vTaskDelete(NULL);
        return;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = 0; // canal atual do STA
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
