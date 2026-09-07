// ---------------------------------------------------------------------------
// ESP32-C3 + ST7735 1.44" (128x128) -- multi-screen homelab dashboard.
//
// This board's WiFi does not associate with normal routers (incomplete PCB
// antenna matching -> deauth reason 2). It runs NO WiFi: all networking
// (CoinGecko, Open-Meteo, MQTT, NTP) happens on the relay ESP32, which sends
// everything over ESP-NOW. This firmware is only screens, buttons, graphics
// and animation.  See ../README.md and ../docs/esp32-c3-wifi-problem.md.
//
// Screens (K1 = GPIO8 prev / K2 = GPIO10 next):
//   BTC/USD | ETH/USD | USD/BRL | Weather | Clock | Meshtastic | Alerts
//   | Printer | Info
//
// LCD pins (Spotpear schematic): SCLK=3 MOSI=4 CS=2 DC=0 RST=5.
// Panel ST7735 128x128 green-tab 1.44. Libs: Adafruit_GFX + Adafruit_ST7735.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "enow_proto.h"  // from ../shared (build_flags -I)

// Optional decorative sprites for the "scene" screens (splash, waiting,
// clock-without-time, empty message screens). Generate them locally with
// tools/img2sprite.py from images YOU have the rights to; they are
// .gitignore'd. When absent, a small code-drawn graphic is used instead.
// The data screens (coin, weather, info) never use a sprite -- there is no
// room in 128px for a 58px opaque block plus a big value plus a sparkline.
#if defined(__has_include) && __has_include("sprites/snoopy_escritor.h") && \
    __has_include("sprites/snoopy_woodstock.h") && \
    __has_include("sprites/charlie_encostado.h")
#  include "sprites/snoopy_escritor.h"
#  include "sprites/snoopy_woodstock.h"
#  include "sprites/charlie_encostado.h"
#  define HAS_SPRITES 1
#endif

// ESP-NOW channel = the router channel the relay uses. If nothing arrives,
// sweep this list until it's found. (The relay last sat on ch 6.)
static const uint8_t BTC_CHANNELS[] = {6, 10, 1, 11};
static const uint32_t SWEEP_MS = 4000;
static const uint32_t LINK_STALE_MS = 150000;   // relay "gone" (2.5 min with no price)
static const uint32_t WEATHER_STALE_MS = 2400000; // weather stale (40 min)
static const uint32_t ALERT_DISPLAY_MS = 8000;
static const uint32_t FRAME_MS = 66;            // ~15 fps
static const uint32_t TWEEN_MS = 700;

#ifndef PIN_TFT_SCLK
#define PIN_TFT_SCLK 3
#define PIN_TFT_MOSI 4
#define PIN_TFT_CS 2
#define PIN_TFT_DC 0
#define PIN_TFT_RST 5
#endif

#define BTN_PREV_PIN 8   // K1
#define BTN_NEXT_PIN 10  // K2
static const uint32_t BTN_DEBOUNCE_MS = 220;

// RGB565 colors
#define C_BLACK ST77XX_BLACK
#define C_WHITE ST77XX_WHITE
#define C_RED 0xF800
#define C_GREEN 0x07E0
#define C_CYAN 0x07FF
#define C_YELLOW 0xFFE0
#define C_ORANGE 0xFD20
#define C_DARKGREY 0x7BEF
#define C_LIGHTGREY 0xC618
#define C_NAVY 0x000F
#define C_GREENY 0x2666
#define C_REDY 0xF9A6

SPIClass spiTFT(FSPI);
static Adafruit_ST7735 tft = Adafruit_ST7735(&spiTFT, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
static GFXcanvas16 cv(128, 128);

// ------------------------------- state -----------------------------------
#define HISTORY_LEN 40
#define WEATHER_HOURLY_LEN 24

enum PriceKind { PRICE_USD, PRICE_BRL_RATE };

struct CoinState
{
    PriceKind kind = PRICE_USD;
    double usd = 0, brl = 0, change24h = 0;
    double displayed = 0;
    bool tweening = false;
    double tweenFrom = 0, tweenTo = 0;
    uint32_t tweenStart = 0;
    float history[HISTORY_LEN] = {0};
    int historyCount = 0;
    String ticker = "  welcome!  ";
    int tickerX = 128, tickerW = 0;
};

static CoinState btcState, ethState, usdtState;
static volatile bool havePrices = false;
static volatile uint32_t lastPriceRx = 0;
static uint32_t pricesSeq = 0;

// clock: the relay sends the UTC epoch (NTP) in the price packet; between
// packets the C3 keeps time with millis(). Brazil = UTC-3, no DST.
#define TZ_OFFSET_S (-3 * 3600)
static volatile uint32_t epochBase = 0;   // UTC epoch of the last packet
static volatile uint32_t epochBaseMs = 0; // millis() when it was received
static volatile bool haveTime = false;

static volatile double wxTemp = 0, wxFeels = 0, wxHum = 0, wxWind = 0, wxMin = 0, wxMax = 0;
static volatile int wxCode = 0;
static volatile bool haveWeather = false;
static volatile uint32_t lastWeatherRx = 0;
static float wxHourly[WEATHER_HOURLY_LEN] = {0};
static int wxHourlyCount = 0;
static String wxTicker = "  weather loading...  ";
static int wxTickerX = 128, wxTickerW = 0;

// MQTT: one ring per category (mesh / alert / print / other) + banner
#define MQTT_LOG_LEN 6
#define MQ_CATS 4
struct MqttMsg { String topic, payload; uint32_t at; };
static MqttMsg mqLog[MQ_CATS][MQTT_LOG_LEN];
static int mqHead[MQ_CATS] = {0}, mqCount[MQ_CATS] = {0};
static uint32_t mqTotal[MQ_CATS] = {0}, mqLastAt[MQ_CATS] = {0};
static uint32_t lastAlertAt = 0;
static uint8_t lastAlertCat = MQ_ALERT;
static uint32_t mqttRxCount = 0; // grand total (all categories)

// ESP-NOW / link
static uint32_t enowRx = 0;
static uint8_t chIdx = 0;           // index into the BTC_CHANNELS sweep
static uint8_t curCh = BTC_CHANNELS[0]; // channel the radio is listening on
static uint8_t relayCh = 0;         // channel stamped by the relay in the last price
static volatile uint8_t relayIp[4] = {0, 0, 0, 0}; // relay's LAN IP (print form)
static bool chLocked = false;
static uint32_t lastSweep = 0;

// animation
static float animPhase = 0;
static uint32_t lastFrame = 0;

// buttons
static int prevBtnLast = HIGH, nextBtnLast = HIGH;
static uint32_t lastBtnAt = 0;

enum Screen { SCR_BTC = 0, SCR_ETH, SCR_USDBRL, SCR_WEATHER, SCR_CLOCK,
              SCR_MESH, SCR_ALERT, SCR_PRINT, SCR_INFO, SCR_COUNT };
static const char *SCR_NAMES[SCR_COUNT] =
    {"BTC", "ETH", "USD/BRL", "Weather", "Clock", "Meshtastic", "Alerts", "Printer", "Info"};
static int screen = SCR_BTC;

// forward
static void renderFrame();

// --------------------------- ESP-NOW recv --------------------------------
static double primaryValue(const CoinState &c) { return c.kind == PRICE_BRL_RATE ? c.brl : c.usd; }

static void pushHistory(CoinState &c, double v)
{
    if (c.historyCount < HISTORY_LEN)
        c.history[c.historyCount++] = (float)v;
    else
    {
        memmove(c.history, c.history + 1, sizeof(float) * (HISTORY_LEN - 1));
        c.history[HISTORY_LEN - 1] = (float)v;
    }
}

static void applyCoin(CoinState &c, double usd, double brl, double chg, bool first)
{
    c.usd = usd; c.brl = brl; c.change24h = chg;
    double v = primaryValue(c);
    pushHistory(c, v);
    if (first) { c.displayed = v; }
    else { c.tweenFrom = c.displayed; c.tweenTo = v; c.tweenStart = millis(); c.tweening = true; }
}

// If the payload is a JSON object, extract something readable (the value of
// a known key, or "k: v  k: v" without braces/quotes). If it's not JSON,
// return it unchanged. A safety net for any MQTT topic.
static String prettyMqtt(String s)
{
    s.trim();
    if (!s.startsWith("{") || !s.endsWith("}"))
        return s;

    static const char *KEYS[] = {"text", "message", "msg",
                                 "state", "status", "value", "payload"};
    for (auto k : KEYS)
    {
        String pat = String("\"") + k + "\"";
        int i = s.indexOf(pat);
        if (i < 0)
            continue;
        int c = s.indexOf(':', i + pat.length());
        if (c < 0)
            continue;
        int j = c + 1;
        while (j < (int)s.length() && s[j] == ' ')
            j++;
        if (j < (int)s.length() && s[j] == '"')
        {
            int q2 = s.indexOf('"', j + 1);
            if (q2 > j)
                return s.substring(j + 1, q2);
        }
        else
        {
            int e = j;
            while (e < (int)s.length() && s[e] != ',' && s[e] != '}')
                e++;
            return s.substring(j, e);
        }
    }
    // no known key: strip the JSON punctuation
    String out;
    for (uint16_t n = 0; n < s.length(); n++)
    {
        char ch = s[n];
        if (ch == '{' || ch == '}' || ch == '"')
            continue;
        if (ch == ',')
        {
            out += "  ";
            continue;
        }
        out += ch;
    }
    out.trim();
    return out.length() ? out : s;
}

static void onRecv(const uint8_t *mac, const uint8_t *data, int len)
{
    if (len < 2 || data[0] != ENOW_MAGIC)
        return;
    enowRx++;

    switch (data[1])
    {
    case ENOW_PRICES:
    {
        if (len != (int)sizeof(enow_prices_t)) return;
        enow_prices_t m; memcpy(&m, data, sizeof(m));
        bool first = !havePrices;
        applyCoin(btcState, m.btc_usd, m.btc_brl, m.btc_chg, first);
        applyCoin(ethState, m.eth_usd, m.eth_brl, m.eth_chg, first);
        applyCoin(usdtState, m.usdt_usd, m.usdt_brl, m.usdt_chg, first);
        havePrices = true;
        lastPriceRx = millis();
        pricesSeq = m.seq;
        if (m.epoch > 1700000000UL) { epochBase = m.epoch; epochBaseMs = millis(); haveTime = true; }
        chLocked = true; // only the price heartbeat (60s) locks the channel
        // follow the relay's WiFi channel (the router hops channels on its own)
        if (m.relay_ch >= 1 && m.relay_ch <= 13)
        {
            relayCh = m.relay_ch;
            if (m.relay_ch != curCh)
            {
                curCh = m.relay_ch;
                esp_wifi_set_channel(curCh, WIFI_SECOND_CHAN_NONE);
            }
        }
        for (int i = 0; i < 4; i++) relayIp[i] = m.relay_ip[i];
        break;
    }
    case ENOW_WEATHER:
    {
        if (len != (int)sizeof(enow_weather_t)) return;
        enow_weather_t m; memcpy(&m, data, sizeof(m));
        wxTemp = m.temp; wxFeels = m.feels; wxHum = m.humidity; wxWind = m.wind;
        wxMin = m.tmin; wxMax = m.tmax; wxCode = m.code;
        wxHourlyCount = m.hourlyCount > WEATHER_HOURLY_LEN ? WEATHER_HOURLY_LEN : m.hourlyCount;
        for (int i = 0; i < wxHourlyCount; i++) wxHourly[i] = m.hourly[i];
        haveWeather = true;
        lastWeatherRx = millis();
        break;
    }
    case ENOW_MQTT:
    {
        if (len != (int)sizeof(enow_mqtt_t)) return;
        enow_mqtt_t m; memcpy(&m, data, sizeof(m));
        m.topic[sizeof(m.topic) - 1] = 0;
        m.payload[sizeof(m.payload) - 1] = 0;
        int c = m.cat < MQ_CATS ? m.cat : MQ_OTHER;
        MqttMsg &slot = mqLog[c][mqHead[c]];
        slot.topic = String(m.topic);
        slot.payload = prettyMqtt(String(m.payload));
        slot.at = millis();
        mqHead[c] = (mqHead[c] + 1) % MQTT_LOG_LEN;
        if (mqCount[c] < MQTT_LOG_LEN) mqCount[c]++;
        mqTotal[c]++;
        mqLastAt[c] = millis();
        mqttRxCount++;
        if (c == MQ_MESH || c == MQ_ALERT) { lastAlertAt = millis(); lastAlertCat = c; }
        break;
    }
    }
}

// --------------------------- text / util -------------------------------
static void txt(int x, int y, uint8_t size, uint16_t color, const String &s)
{
    cv.setTextSize(size);
    cv.setTextColor(color);
    cv.setCursor(x, y);
    cv.print(s);
}

static int textW(const String &s, uint8_t size)
{
    cv.setTextSize(size);
    int16_t bx, by; uint16_t bw, bh;
    cv.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
    return bw;
}

static void centered(const String &s, int y, uint8_t size, uint16_t color)
{
    txt((128 - textW(s, size)) / 2, y, size, color, s);
}

// draws 'text', breaking on '\n' and then by length (cpl chars/line),
// up to 'maxLines'. Returns how many lines it drew.
static int drawWrapped(int x, int y, int cpl, int maxLines, uint16_t color, const String &text)
{
    int drawn = 0, start = 0;
    while (drawn < maxLines && start <= (int)text.length())
    {
        int nl = text.indexOf('\n', start);
        String seg = (nl < 0) ? text.substring(start) : text.substring(start, nl);
        int pos = 0;
        while (drawn < maxLines && (pos < (int)seg.length() || (seg.length() == 0 && nl >= 0 && pos == 0)))
        {
            txt(x, y + drawn * 10, 1, color, seg.substring(pos, pos + cpl));
            pos += cpl;
            drawn++;
            if (seg.length() == 0) break;
        }
        if (nl < 0) break;
        start = nl + 1;
    }
    return drawn;
}

static String fmtThousands(long v)
{
    String s = String(v);
    for (int i = (int)s.length() - 3; i > 0; i -= 3)
        s = s.substring(0, i) + "." + s.substring(i);
    return s;
}
static String fmtUsd(double v) { return "$" + fmtThousands((long)(v + 0.5)); }
static String fmtBrl(double v) { return "R$ " + fmtThousands((long)(v + 0.5)); }
static String fmtBrlRate(double v)
{
    char b[16]; snprintf(b, sizeof(b), "R$ %.2f", v);
    String s(b); s.replace('.', ','); return s;
}

// --------------------------- graphics ----------------------------------
static void drawSparkline(int x, int y, int w, int h, float *hist, int n, int cap, float minRange)
{
    cv.drawRect(x, y, w, h, C_LIGHTGREY);
    if (n == 1) { cv.fillCircle(x + w / 2, y + h / 2, 2, C_WHITE); return; }
    if (n < 2) return;

    float mn = hist[0], mx = hist[0];
    for (int i = 1; i < n; i++) { if (hist[i] < mn) mn = hist[i]; if (hist[i] > mx) mx = hist[i]; }
    if (mx - mn < minRange) mx = mn + minRange;

    int iw = w - 4, ih = h - 4, px = -1, py = 0;
    for (int i = 0; i < n; i++)
    {
        int cx = x + 2 + (iw * i) / (cap - 1);
        int cy = y + 2 + ih - (int)((hist[i] - mn) / (mx - mn) * ih);
        if (px >= 0)
            cv.drawLine(px, py, cx, cy, hist[i] >= hist[i - 1] ? C_GREEN : C_RED);
        px = cx; py = cy;
    }
}

static void drawArrow(int x, int y, bool up, uint16_t color)
{
    if (up) cv.fillTriangle(x + 5, y, x, y + 9, x + 10, y + 9, color);
    else    cv.fillTriangle(x, y, x + 10, y, x + 5, y + 9, color);
}

// scale the brightness of an RGB565 color (0..1)
static uint16_t sc565(uint16_t c, float b)
{
    b = constrain(b, 0.0f, 1.0f);
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * b);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * b);
    uint8_t bl = (uint8_t)((c & 0x1F) * b);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// ------------------------- Snoopy / Peanuts theme --------------------
#define C_PNUT_RED 0xC060   // doghouse red
#define C_PNUT_CREAM 0xFF9C // speech-bubble cream

// Optional sprites generated by tools/img2sprite.py (from an image you have
// the rights to use). If the headers exist, they replace the geometric
// drawing. `snoopy` = large (doghouse/splash), `snoopyMini` = title bar.
#if defined(__has_include)
#  if __has_include("snoopy.h")
#    include "snoopy.h"
#    define HAS_SNOOPY_BIG 1
#  endif
#  if __has_include("snoopyMini.h")
#    include "snoopyMini.h"
#    define HAS_SNOOPY_MINI 1
#  endif
#endif

// blit an opaque RGB565 sprite (the src/sprites/ headers have no mask --
// they're full rectangles), centered on the x axis. GFXcanvas16's
// drawRGBBitmap clips whatever runs past the edge on its own.
static void blitSpriteCx(int y, const uint16_t *data, int w, int h)
{
    cv.drawRGBBitmap((128 - w) / 2, y, data, w, h);
}

// Snoopy in profile (facing right), black on a LIGHT background.
// ~18x14 at scale 1. Made to sit inside the white title bubble.
static void drawMiniSnoopy(int x, int y)
{
#ifdef HAS_SNOOPY_MINI
    cv.drawRGBBitmap(x, y, (uint16_t *)snoopyMini_data, (uint8_t *)snoopyMini_mask,
                     snoopyMini_W, snoopyMini_H);
    return;
#endif
    // head
    cv.fillCircle(x + 6, y + 6, 6, C_BLACK);
    cv.fillCircle(x + 6, y + 6, 4, C_WHITE);
    // muzzle
    cv.fillRoundRect(x + 9, y + 5, 8, 6, 2, C_BLACK);
    cv.fillRoundRect(x + 9, y + 6, 6, 4, 2, C_WHITE);
    // nose
    cv.fillCircle(x + 16, y + 8, 2, C_BLACK);
    // floppy ear
    cv.fillRoundRect(x, y + 4, 5, 9, 2, C_BLACK);
    // eye
    cv.fillCircle(x + 6, y + 5, 1, C_BLACK);
}

// Snoopy's red doghouse.
static void drawDoghouse(int x, int y, int w, int h)
{
    int roofH = h * 2 / 5;
    cv.fillRect(x, y + roofH, w, h - roofH, C_PNUT_RED);
    cv.fillTriangle(x - 2, y + roofH + 1, x + w / 2, y, x + w + 2, y + roofH + 1, C_PNUT_RED);
    cv.drawLine(x - 2, y + roofH + 1, x + w / 2, y, C_WHITE);
    cv.drawLine(x + w / 2, y, x + w + 2, y + roofH + 1, C_WHITE);
    // arched door
    int dw = w / 3, dx = x + (w - dw) / 2, dy = y + h - (h - roofH) * 3 / 4;
    cv.fillRoundRect(dx, dy, dw, y + h - dy, 3, C_BLACK);
}

// Snoopy lying on his back on the roof (the classic pose),
// with "Z Z Z". x,y = top-left corner of a ~72x46 area.
static void drawSnoopyLounging(int x, int y)
{
#ifdef HAS_SNOOPY_BIG
    cv.drawRGBBitmap(x + (72 - snoopy_W) / 2, y, (uint16_t *)snoopy_data,
                     (uint8_t *)snoopy_mask, snoopy_W, snoopy_H);
    return;
#endif
    drawDoghouse(x + 8, y + 10, 56, 34);
    // Snoopy's body lying along the ridge
    int bx = x + 12, by = y + 8;
    cv.fillRoundRect(bx, by, 40, 9, 4, C_WHITE);
    cv.drawRoundRect(bx, by, 40, 9, 4, C_LIGHTGREY);
    // head hanging off the right end
    cv.fillCircle(bx + 44, by + 6, 6, C_WHITE);
    cv.drawCircle(bx + 44, by + 6, 6, C_LIGHTGREY);
    cv.fillRoundRect(bx + 45, by + 4, 7, 5, 2, C_WHITE); // muzzle
    cv.fillCircle(bx + 51, by + 6, 1, C_BLACK);          // nose
    cv.fillRoundRect(bx + 44, by + 8, 4, 8, 2, C_BLACK); // floppy ear
    // paws up
    for (int i = 0; i < 4; i++)
        cv.fillRoundRect(bx + 4 + i * 9, by - 5, 4, 6, 2, C_WHITE);
    // Z Z Z
    txt(x + 2, y + 2, 1, C_YELLOW, "z");
    txt(x + 8, y - 2, 2, C_YELLOW, "z");
}

// Comic-strip style title bubble: rounded white rectangle + mini
// Snoopy + black label. Returns the y at the bottom of the bubble.
static int drawTitleBar(const String &label, uint16_t accent)
{
    int w = 24 + textW(label, 1) + 8;
    if (w > 126) w = 126;
    int x = (128 - w) / 2, y = 1, h = 16;
    cv.fillRoundRect(x, y, w, h, 5, C_WHITE);
    // border "breathes"
    float b = 0.55f + 0.45f * (0.5f + 0.5f * sinf(animPhase));
    cv.drawRoundRect(x, y, w, h, 5, sc565(accent, b));
    drawMiniSnoopy(x + 3, y + 1);
    txt(x + 22, y + 5, 1, C_BLACK, label);
    return y + h + 2; // first free y
}

// --------------------------- icons (geometric) ---------------------
typedef void (*IconFn)(int x, int y, float b);

static void drawBtcIcon(int x, int y, float b)
{
    uint16_t o = sc565(C_ORANGE, b), w = sc565(C_WHITE, b);
    int cx = x + 14, cy = y + 14;
    cv.fillCircle(cx, cy, 13, o);
    // stylized "B" + the vertical strokes of the ₿ symbol
    cv.fillRect(cx - 5, cy - 8, 3, 16, w);
    cv.fillRect(cx - 5, cy - 8, 8, 3, w);
    cv.fillRect(cx - 5, cy - 1, 8, 3, w);
    cv.fillRect(cx - 5, cy + 5, 8, 3, w);
    cv.fillRect(cx + 1, cy - 6, 3, 6, w);
    cv.fillRect(cx + 1, cy + 1, 3, 6, w);
    cv.fillRect(cx - 2, cy - 11, 2, 4, w);
    cv.fillRect(cx + 2, cy - 11, 2, 4, w);
    cv.fillRect(cx - 2, cy + 8, 2, 4, w);
    cv.fillRect(cx + 2, cy + 8, 2, 4, w);
}

static void drawEthIcon(int x, int y, float b)
{
    uint16_t hi = sc565(0x9CDF, b), lo = sc565(0x4C1F, b);
    int cx = x + 14;
    cv.fillTriangle(cx, y + 1, x + 2, y + 15, cx, y + 16, hi);
    cv.fillTriangle(cx, y + 1, x + 26, y + 15, cx, y + 16, lo);
    cv.fillTriangle(x + 2, y + 18, cx, y + 27, x + 26, y + 18, hi);
    cv.fillTriangle(x + 2, y + 18, cx, y + 21, x + 26, y + 18, lo);
}

static void drawUsdIcon(int x, int y, float b)
{
    uint16_t coin = sc565(0x2665, b), edge = sc565(0x6F2C, b);
    int cx = x + 14, cy = y + 14;
    cv.fillCircle(cx, cy, 13, coin);
    cv.drawCircle(cx, cy, 13, edge);
    cv.drawCircle(cx, cy, 12, edge);
    cv.setTextSize(2);
    cv.setTextColor(sc565(C_WHITE, b));
    cv.setCursor(cx - 5, cy - 7);
    cv.print("$");
}

// --------------------------- weather icons ------------------------
enum WxCat { WX_CLEAR, WX_CLOUD, WX_RAIN, WX_STORM, WX_SNOW, WX_FOG };
static WxCat wxCategory(int code)
{
    if (code == 0) return WX_CLEAR;
    if (code == 1 || code == 2 || code == 3) return WX_CLOUD;
    if (code == 45 || code == 48) return WX_FOG;
    if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86) return WX_SNOW;
    if (code == 95 || code == 96 || code == 99) return WX_STORM;
    return WX_RAIN;
}

static void drawCloud(int x, int y, uint16_t color)
{
    cv.fillCircle(x + 9, y + 12, 6, color);
    cv.fillCircle(x + 16, y + 9, 7, color);
    cv.fillCircle(x + 22, y + 12, 5, color);
    cv.fillRoundRect(x + 4, y + 12, 20, 8, 4, color);
}

static void drawWeatherIcon(int x, int y, int code)
{
    switch (wxCategory(code))
    {
    case WX_CLEAR:
    {
        int cx = x + 14, cy = y + 13;
        cv.fillCircle(cx, cy, 7, C_YELLOW);
        for (int i = 0; i < 8; i++)
        {
            float a = i * (float)(PI / 4.0);
            cv.drawLine(cx + (int)(cosf(a) * 10), cy + (int)(sinf(a) * 10),
                        cx + (int)(cosf(a) * 13), cy + (int)(sinf(a) * 13), C_YELLOW);
        }
        break;
    }
    case WX_CLOUD: drawCloud(x, y, C_LIGHTGREY); break;
    case WX_RAIN:
        drawCloud(x, y, C_LIGHTGREY);
        for (int i = 0; i < 3; i++) { int lx = x + 7 + i * 7; cv.drawLine(lx, y + 21, lx - 3, y + 27, C_CYAN); }
        break;
    case WX_STORM:
        drawCloud(x, y, C_DARKGREY);
        cv.fillTriangle(x + 14, y + 18, x + 10, y + 24, x + 15, y + 23, C_YELLOW);
        cv.fillTriangle(x + 15, y + 23, x + 12, y + 27, x + 18, y + 20, C_YELLOW);
        break;
    case WX_SNOW:
        drawCloud(x, y, C_LIGHTGREY);
        for (int i = 0; i < 3; i++)
        {
            int cx = x + 8 + i * 7, cy = y + 24;
            cv.drawLine(cx - 2, cy, cx + 2, cy, C_WHITE);
            cv.drawLine(cx, cy - 2, cx, cy + 2, C_WHITE);
        }
        break;
    case WX_FOG:
        for (int i = 0; i < 4; i++)
        {
            int ly = y + 6 + i * 6, lw = (i % 2 == 0) ? 22 : 16;
            cv.drawFastHLine(x + (28 - lw) / 2, ly, lw, C_LIGHTGREY);
        }
        break;
    }
}

// number + "°C" (the degree symbol is a little hand-drawn circle)
static void printDegrees(int x, int y, double v, uint8_t size, uint16_t color)
{
    char b[8]; snprintf(b, sizeof(b), "%.0f", v);
    txt(x, y, size, color, b);
    int cx = cv.getCursorX() + 3, cy = y + 4 * size - 2;
    cv.drawCircle(cx, cy, 2, color);
    txt(cx + 6, y, size, color, "C");
}

// --------------------------- tickers ------------------------------------
static void updateCoinTicker(CoinState &c)
{
    uint32_t ago = (millis() - lastPriceRx) / 1000;
    char b[64];
    if (c.kind == PRICE_BRL_RATE)
        snprintf(b, sizeof(b), "  USDT ~ $%.2f  -  updated %lus ago  ", c.usd, (unsigned long)ago);
    else
        snprintf(b, sizeof(b), "  %s  -  updated %lus ago  ", fmtBrl(c.brl).c_str(), (unsigned long)ago);
    c.ticker = String(b);
    c.tickerW = textW(c.ticker, 1);
}

static void updateWxTicker()
{
    uint32_t ago = (millis() - lastWeatherRx) / 1000;
    char b[96];
    snprintf(b, sizeof(b), "  min %.0fC max %.0fC   hum %.0f%%   wind %.0fkm/h  -  %lus ago  ",
             wxMin, wxMax, wxHum, wxWind, (unsigned long)ago);
    wxTicker = String(b);
    wxTickerW = textW(wxTicker, 1);
}

// --------------------------- screens -----------------------------------
static bool linkStale() { return !havePrices || millis() - lastPriceRx > LINK_STALE_MS; }

static void renderCoinScreen(CoinState &c, const char *label, IconFn icon)
{
    (void)icon;
    drawTitleBar(label, C_ORANGE);

    if (havePrices)
    {
        float blink = 0.5f + 0.5f * sinf(animPhase * 1.6f);
        if (blink > 0.25f)
            drawArrow(115, 20, c.change24h >= 0, c.change24h >= 0 ? C_GREEN : C_RED);
        uint16_t chc = c.change24h >= 0 ? C_GREEN : C_RED;
        char b[24];
        snprintf(b, sizeof(b), "24h %s%.2f%%", c.change24h >= 0 ? "+" : "", c.change24h);
        txt(4, 22, 1, linkStale() ? C_DARKGREY : chc, b);
    }

    double shown = c.displayed;
    if (c.tweening)
    {
        float t = (float)(millis() - c.tweenStart) / (float)TWEEN_MS;
        if (t >= 1.0f) { t = 1.0f; c.tweening = false; c.displayed = c.tweenTo; }
        shown = c.tweenFrom + (c.tweenTo - c.tweenFrom) * t;
    }

    String big = !havePrices ? "..."
                 : (c.kind == PRICE_BRL_RATE ? fmtBrlRate(shown) : fmtUsd(shown));
    uint8_t sz = 3;
    if (textW(big, 3) > 124) sz = 2;
    centered(big, 36, sz, linkStale() ? C_DARKGREY : C_WHITE);

    drawSparkline(4, 66, 120, 30, c.history, c.historyCount, HISTORY_LEN,
                  c.kind == PRICE_BRL_RATE ? 0.01f : 1.0f);

    cv.drawFastHLine(0, 104, 128, C_PNUT_RED);
    cv.drawFastHLine(0, 106, 128, C_PNUT_RED);
    txt(c.tickerX, 112, 1, C_WHITE, c.ticker);
}

static void renderWeatherScreen()
{
    drawTitleBar("Weather", C_CYAN);
    drawWeatherIcon(4, 22, wxCode);

    bool stale = !haveWeather || millis() - lastWeatherRx > WEATHER_STALE_MS;
    if (haveWeather)
    {
        printDegrees(40, 26, wxTemp, 3, stale ? C_DARKGREY : C_WHITE);
        char b[24]; snprintf(b, sizeof(b), "feels %.0fC", wxFeels);
        txt(4, 54, 1, stale ? C_DARKGREY : C_CYAN, b);
    }
    else
        txt(40, 26, 3, C_WHITE, "...");

    drawSparkline(4, 66, 120, 30, wxHourly, wxHourlyCount, WEATHER_HOURLY_LEN, 1.0f);
    cv.drawFastHLine(0, 104, 128, C_PNUT_RED);
    cv.drawFastHLine(0, 106, 128, C_PNUT_RED);
    txt(wxTickerX, 112, 1, C_WHITE, wxTicker);
}

static void renderMsgScreen(int cat, const char *title, uint16_t titleColor, const char *emptyMsg)
{
    drawTitleBar(title, titleColor);
    if (cat == MQ_PRINT)
    {
        // the relay's LAN IP = where the "type text to print" form lives
        char ipb[26];
        if (relayIp[0] || relayIp[1] || relayIp[2] || relayIp[3])
            snprintf(ipb, sizeof(ipb), "> %u.%u.%u.%u", (unsigned)relayIp[0],
                     (unsigned)relayIp[1], (unsigned)relayIp[2], (unsigned)relayIp[3]);
        else
            snprintf(ipb, sizeof(ipb), "> no IP (relay?)");
        txt(4, 22, 1, C_CYAN, ipb);
    }
    else
    {
        char h[20]; snprintf(h, sizeof(h), "%lu received", (unsigned long)mqTotal[cat]);
        txt(128 - textW(h, 1) - 2, 22, 1, C_DARKGREY, h);
    }

    if (mqCount[cat] == 0)
    {
#ifdef HAS_SPRITES
        const uint16_t *sd; int sw, sh;
        switch (cat)
        {
        case MQ_MESH:
            sd = snoopy_woodstock_data; sw = snoopy_woodstock_W; sh = snoopy_woodstock_H; break;
        case MQ_PRINT:
            sd = snoopy_escritor_data; sw = snoopy_escritor_W; sh = snoopy_escritor_H; break;
        default:
            sd = charlie_encostado_data; sw = charlie_encostado_W; sh = charlie_encostado_H; break;
        }
        blitSpriteCx(30, sd, sw, sh);
        centered(emptyMsg, 92, 1, C_DARKGREY);
#else
        centered(emptyMsg, 60, 1, C_DARKGREY);
#endif
        return;
    }
    int y = 34;
    for (int i = 0; i < mqCount[cat] && y < 116; i++)
    {
        int idx = (mqHead[cat] - 1 - i + MQTT_LOG_LEN * 2) % MQTT_LOG_LEN;
        MqttMsg &m = mqLog[cat][idx];
        uint32_t ago = (millis() - m.at) / 1000;
        char tb[8];
        if (ago < 90) snprintf(tb, sizeof(tb), "%lus ago", (unsigned long)ago);
        else snprintf(tb, sizeof(tb), "%lum ago", (unsigned long)(ago / 60));
        txt(128 - textW(tb, 1) - 2, y, 1, C_DARKGREY, tb);
        // 1st msg: up to 4 lines; older ones 2
        int lines = drawWrapped(4, y, 21, i == 0 ? 4 : 2, i == 0 ? C_WHITE : C_LIGHTGREY, m.payload);
        y += lines * 10 + 4;
        if (i == 0) cv.drawFastHLine(0, y - 2, 128, 0x2104);
    }
}

static void renderInfoScreen()
{
    drawTitleBar("Info", C_LIGHTGREY);
    int y = 24;
    auto line = [&](const String &s, uint16_t c) { txt(4, y, 1, c, s); y += 11; };

    line(String("link: ") + (chLocked ? "OK ch " + String(curCh) : "searching"),
         chLocked ? C_GREEN : C_YELLOW);
    line(String("ESP-NOW packets: ") + String((unsigned long)enowRx), C_WHITE);
    if (havePrices)
    {
        uint32_t ago = (millis() - lastPriceRx) / 1000;
        line(String("last price: ") + String((unsigned long)ago) + "s",
             linkStale() ? C_RED : C_WHITE);
        line(String("seq: ") + String((unsigned long)pricesSeq), C_DARKGREY);
    }
    else
        line("price: waiting for relay", C_YELLOW);
    line(String("weather: ") + (haveWeather ? "ok" : "waiting"), haveWeather ? C_WHITE : C_YELLOW);
    line(String("time: ") + (haveTime ? "synced" : "waiting NTP"),
         haveTime ? C_WHITE : C_YELLOW);
    line(String("mesh ") + String((unsigned long)mqTotal[MQ_MESH]) +
             "  alert " + String((unsigned long)mqTotal[MQ_ALERT]) +
             "  print " + String((unsigned long)mqTotal[MQ_PRINT]),
         C_WHITE);
    line(String("heap: ") + String(ESP.getFreeHeap() / 1024) + " KB", C_DARKGREY);
    line(String("up: ") + String((unsigned long)(millis() / 1000)) + "s", C_DARKGREY);
}

static void drawAlertOverlay()
{
    int c = lastAlertCat;
    if (lastAlertAt == 0 || millis() - lastAlertAt > ALERT_DISPLAY_MS || mqCount[c] == 0)
        return;
    int idx = (mqHead[c] - 1 + MQTT_LOG_LEN) % MQTT_LOG_LEN;
    MqttMsg &m = mqLog[c][idx];

    int y = 28, h = 72;
    cv.fillRect(0, y, 128, h, C_NAVY);
    cv.drawRect(0, y, 128, h, C_YELLOW);

    txt(4, y + 4, 1, C_YELLOW, c == MQ_MESH ? "Meshtastic" : "Alert");
    drawWrapped(4, y + 18, 21, 5, C_WHITE, m.payload);
}

// --------------------------- clock screen --------------------------
static const char *WEEKDAY[7] =
    {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
static const char *MONTH_NAME[12] =
    {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
     "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};

static void renderClockScreen()
{
    drawTitleBar("Clock", C_PNUT_RED);

    if (!haveTime)
    {
        centered("waiting for time", 54, 1, C_WHITE);
        centered("(NTP via relay)", 68, 1, C_DARKGREY);
        int dots = (millis() / 300) % 16;
        String d; for (int i = 0; i < dots; i++) d += ".";
        centered(d, 84, 1, C_DARKGREY);
        return;
    }

    uint32_t utc = epochBase + (millis() - epochBaseMs) / 1000;
    time_t local = (time_t)utc + TZ_OFFSET_S;
    struct tm t;
    gmtime_r(&local, &t);

    // weekday
    centered(WEEKDAY[t.tm_wday % 7], 26, 2, C_ORANGE);

    // big HH:MM (size 4 = 24px/char). ':' blinks (swapped for ' ' -> fixed width)
    char hm[8];
    snprintf(hm, sizeof(hm), "%02d%c%02d", t.tm_hour, (t.tm_sec & 1) ? ':' : ' ', t.tm_min);
    txt((128 - 5 * 24) / 2, 52, 4, C_WHITE, hm);

    // seconds, centered
    char ss[6];
    snprintf(ss, sizeof(ss), "%02d s", t.tm_sec);
    centered(ss, 88, 1, C_CYAN);

    // full date
    char dt[28];
    snprintf(dt, sizeof(dt), "%02d %s %04d", t.tm_mday, MONTH_NAME[t.tm_mon % 12], t.tm_year + 1900);
    centered(dt, 102, 1, C_LIGHTGREY);

    // minute bar (seconds/60)
    int w = t.tm_sec * 116 / 60;
    cv.drawRect(6, 116, 116, 6, C_PNUT_RED);
    if (w > 0) cv.fillRect(6, 116, w, 6, C_PNUT_RED);

    uint32_t sinceSync = (millis() - epochBaseMs) / 1000;
    if (sinceSync > 300)
        centered("no sync for " + String(sinceSync / 60) + " min", 108, 1, C_RED);
}

static void renderWaiting()
{
    drawTitleBar("Dashboard", C_PNUT_RED);
#ifdef HAS_SPRITES
    blitSpriteCx(22, snoopy_escritor_data, snoopy_escritor_W, snoopy_escritor_H);
#else
    drawSnoopyLounging(28, 30);
#endif
    centered("waiting for the relay", 88, 1, C_WHITE);
    centered(String("ESP-NOW ch") + String(curCh), 102, 1, C_DARKGREY);
    int dots = (millis() / 300) % 16;
    String d = ""; for (int i = 0; i < dots; i++) d += ".";
    centered(d, 116, 1, C_DARKGREY);
}

static void renderFrame()
{
    cv.fillScreen(C_BLACK);

    if (screen == SCR_BTC && !havePrices)
        renderWaiting();
    else
    {
        switch (screen)
        {
        case SCR_BTC:    renderCoinScreen(btcState, "BTC/USD", drawBtcIcon); break;
        case SCR_ETH:    renderCoinScreen(ethState, "ETH/USD", drawEthIcon); break;
        case SCR_USDBRL: renderCoinScreen(usdtState, "USD/BRL", drawUsdIcon); break;
        case SCR_WEATHER:  renderWeatherScreen(); break;
        case SCR_CLOCK: renderClockScreen(); break;
        case SCR_MESH:   renderMsgScreen(MQ_MESH, "Meshtastic", C_GREEN, "no messages"); break;
        case SCR_ALERT:  renderMsgScreen(MQ_ALERT, "Alerts", C_YELLOW, "no alerts"); break;
        case SCR_PRINT:  renderMsgScreen(MQ_PRINT, "Printer", C_CYAN, "no status"); break;
        case SCR_INFO:   renderInfoScreen(); break;
        }
        // footer with the screen name/position on screens without their own ticker
        if (screen >= SCR_MESH)
        {
            char pb[16]; snprintf(pb, sizeof(pb), "%s %d/%d", SCR_NAMES[screen], screen + 1, SCR_COUNT);
            txt(4, 118, 1, C_DARKGREY, pb);
        }
    }

    drawAlertOverlay();
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 128, 128);
}

static void advanceAnim()
{
    animPhase += 0.35f;
    if (animPhase > 6283.0f) animPhase -= 6283.0f;

    CoinState *cs[] = {&btcState, &ethState, &usdtState};
    for (CoinState *c : cs)
    {
        c->tickerX -= 2;
        if (c->tickerX < -c->tickerW) c->tickerX = 128;
    }
    wxTickerX -= 2;
    if (wxTickerX < -wxTickerW) wxTickerX = 128;
}

static void handleButtons()
{
    int p = digitalRead(BTN_PREV_PIN), n = digitalRead(BTN_NEXT_PIN);
    uint32_t now = millis();
    if (prevBtnLast == HIGH && p == LOW && now - lastBtnAt > BTN_DEBOUNCE_MS)
    {
        screen = (screen - 1 + SCR_COUNT) % SCR_COUNT;
        lastBtnAt = now; renderFrame();
    }
    else if (nextBtnLast == HIGH && n == LOW && now - lastBtnAt > BTN_DEBOUNCE_MS)
    {
        screen = (screen + 1) % SCR_COUNT;
        lastBtnAt = now; renderFrame();
    }
    prevBtnLast = p; nextBtnLast = n;
}

static void channelTask()
{
    // if it's locked but the price went stale (relay changed channel / dropped),
    // unlock and start sweeping again
    if (chLocked && havePrices && millis() - lastPriceRx > LINK_STALE_MS)
    {
        chLocked = false;
        Serial.println("ESP-NOW: price stale -> re-sweeping channels");
    }
    if (chLocked) return;
    if (millis() - lastSweep < SWEEP_MS) return;
    lastSweep = millis();
    chIdx = (chIdx + 1) % (sizeof(BTC_CHANNELS) / sizeof(BTC_CHANNELS[0]));
    curCh = BTC_CHANNELS[chIdx];
    esp_wifi_set_channel(curCh, WIFI_SECOND_CHAN_NONE);
    Serial.printf("ESP-NOW: trying channel %d\n", curCh);
}

// --------------------------- setup/loop -------------------------------
void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\nESP32-C3 dashboard (ESP-NOW)");

    pinMode(BTN_PREV_PIN, INPUT_PULLUP);
    pinMode(BTN_NEXT_PIN, INPUT_PULLUP);
    usdtState.kind = PRICE_BRL_RATE;

    spiTFT.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
    tft.initR(INITR_144GREENTAB);
    tft.setRotation(2);

    // display self-test
    tft.fillScreen(C_RED); delay(250);
    tft.fillScreen(C_GREEN); delay(250);
    tft.fillScreen(0x001F); delay(250);
    tft.fillScreen(C_BLACK);
    cv.setTextWrap(false);

    // boot splash
    cv.fillScreen(C_BLACK);
    drawTitleBar("Dashboard", C_PNUT_RED);
#ifdef HAS_SPRITES
    blitSpriteCx(24, snoopy_escritor_data, snoopy_escritor_W, snoopy_escritor_H);
#else
    drawSnoopyLounging(28, 34);
#endif
    centered("ESP-NOW", 100, 1, C_DARKGREY);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 128, 128);
    delay(1600);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    esp_wifi_set_channel(curCh, WIFI_SECOND_CHAN_NONE);
    Serial.printf("MAC=%s  start channel=%d\n", WiFi.macAddress().c_str(), curCh);

    if (esp_now_init() != ESP_OK)
        Serial.println("esp_now_init FAILED");
    else
    {
        esp_now_register_recv_cb(onRecv);
        Serial.println("ESP-NOW ready, waiting for the relay...");
    }

    renderFrame();
}

void loop()
{
    handleButtons();
    channelTask();

    if (millis() - lastFrame >= FRAME_MS)
    {
        lastFrame = millis();
        advanceAnim();
        if (havePrices)
        {
            updateCoinTicker(btcState);
            updateCoinTicker(ethState);
            updateCoinTicker(usdtState);
        }
        if (haveWeather) updateWxTicker();
        renderFrame();
    }

    static uint32_t lastLog = 0;
    if (millis() - lastLog > 10000)
    {
        lastLog = millis();
        char hhmm[10] = "--:--:--";
        if (haveTime)
        {
            time_t lt = (time_t)(epochBase + (millis() - epochBaseMs) / 1000) + TZ_OFFSET_S;
            struct tm t; gmtime_r(&lt, &t);
            snprintf(hhmm, sizeof(hhmm), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        }
        Serial.printf("rx=%lu ch=%d lock=%d btc=%.0f eth=%.0f usdbrl=%.3f wx=%.1fC mqtt=%lu time=%s heap=%u\n",
                      (unsigned long)enowRx, curCh, (int)chLocked,
                      btcState.usd, ethState.usd, usdtState.brl, (double)wxTemp,
                      (unsigned long)mqttRxCount, hhmm, ESP.getFreeHeap());
    }
    delay(5);
}
