// ============================================================================
//  ESP32  <->  Epson LX-810L
//  Bridge: everything that arrives on Serial (USB) is printed on the
//  parallel port. This file also wires in the ESP-NOW relay (btc_relay),
//  the web server (web_print) and the port-9100 printer (raw_print), and
//  produces the printer-status string that btc_relay publishes to MQTT.
//
//  - TEXT MODE (default): CR, LF and CRLF all become CRLF, so terminal text
//    prints with correct line breaks.
//  - RAW MODE (-D PRN_TEXT_MODE=0): stream untouched, for binary ESC/P.
//
//  On-board LED:
//    off      -> idle
//    on       -> printing / data in the queue
//    blinking -> printer paused (offline, out of paper, error, or BUSY stuck)
// ============================================================================
#include <Arduino.h>
#include "config.h"
#include "CentronicsPrinter.h"
#include "print_queue.h"
#include "btc_relay.h"
#include "web_print.h"
#include "raw_print.h"

static CentronicsPrinter printer;

// ---- ring buffer between Serial (USB) and the printer ---------------------
static uint8_t  ring[PRN_RING_SIZE];
static size_t   rHead = 0, rTail = 0;

static inline size_t  ringCount() { return (rHead - rTail) & (PRN_RING_SIZE - 1); }
static inline size_t  ringFree()  { return PRN_RING_SIZE - 1 - ringCount(); }
static inline void    ringPush(uint8_t b) { ring[rHead] = b; rHead = (rHead + 1) & (PRN_RING_SIZE - 1); }
static inline uint8_t ringPeek()  { return ring[rTail]; }
static inline void    ringPop()   { rTail = (rTail + 1) & (PRN_RING_SIZE - 1); }

static bool xoffSent = false;

#if PRN_TEXT_MODE
static bool sawCR = false;      // we just pushed a CRLF because of a CR
#endif

#if defined(PRN_DEBUG)
// ---- bench commands (type the line + Enter) ----
//   @DIAG        full self-test (sweeps D0..D7, pulses /STROBE)
//   @PINS        shows SELECT/BUSY/PE/ERROR for 8 s (toggle the printer's On Line)
//   @D0 .. @D7   force just that data line HIGH (measure on DB25 2..9)
//   @DX          clear the data bus
//   @ST          one slow /STROBE pulse, watching BUSY
#include <string.h>
static char g_cmd[12];
static uint8_t g_cmdn = 0;

static void runCmd(const char* s) {
    if (!strcmp(s, "@DIAG")) { printer.diagnostics(Serial); return; }
    if (!strcmp(s, "@PINS")) {
        Serial.println(F("[PINS] 8 s - toggle On Line / cover / paper and watch:"));
        const uint32_t t0 = millis();
        while (millis() - t0 < 8000) {
            Serial.print(F("  SELECT=")); Serial.print(digitalRead(PIN_SELECT));
            Serial.print(F("  BUSY="));   Serial.print(digitalRead(PIN_BUSY));
            Serial.print(F("  PE="));     Serial.print(digitalRead(PIN_PE));
            Serial.print(F("  ERROR="));  Serial.println(digitalRead(PIN_ERROR));
            delay(400);
        }
        return;
    }
    if (s[0] == '@' && s[1] == 'D' && s[2] >= '0' && s[2] <= '7' && s[3] == 0) {
        printer.dbgSetData((uint8_t)(1u << (s[2] - '0')));
        Serial.print(F("[D")); Serial.print(s[2]);
        Serial.print(F("] HIGH -> measure on DB25 pin ")); Serial.print(s[2] - '0' + 2);
        Serial.println(F(" (~5 V). '@DX' clears."));
        return;
    }
    if (!strcmp(s, "@DX")) { printer.dbgSetData(0); Serial.println(F("[DX] data = 0")); return; }
    if (!strcmp(s, "@ST")) {
        const bool a = printer.busy();
        printer.dbgStrobe(5);
        delay(15);
        const bool b = printer.busy();
        Serial.print(F("[ST] BUSY before=")); Serial.print(a);
        Serial.print(F("  after=")); Serial.println(b);
        return;
    }
}

static void feedCmd(uint8_t c) {
    if (c == '\r' || c == '\n') {
        g_cmd[g_cmdn] = 0;
        if (g_cmdn >= 3 && g_cmd[0] == '@') runCmd(g_cmd);
        g_cmdn = 0;
    } else if (g_cmdn < sizeof(g_cmd) - 1) {
        g_cmd[g_cmdn++] = (char) c;
    }
}
#endif

// ---------------------------------------------------------------------------
static void pumpSerialIntoRing() {
    // leave 2 bytes of headroom (TEXT MODE can push a CRLF at once)
    while (Serial.available() && ringFree() > 2) {
        const uint8_t c = (uint8_t) Serial.read();
#if defined(PRN_DEBUG)
        feedCmd(c);
#endif
#if PRN_TEXT_MODE
        if (c == '\r') { ringPush('\r'); ringPush('\n'); sawCR = true;  continue; }
        if (c == '\n') { if (sawCR) { sawCR = false; continue; }        // swallow the \n of a CRLF
                         ringPush('\r'); ringPush('\n'); continue; }
        sawCR = false;
        ringPush(c);
#else
        ringPush(c);
#endif
    }
}

// ---------------------------------------------------------------------------
static void updateFlowControl() {
#if PRN_XONXOFF
    if (!xoffSent && ringCount() > (PRN_RING_SIZE * 3 / 4)) {
        Serial.write(0x13);        // XOFF -> host, stop sending
        xoffSent = true;
    } else if (xoffSent && ringCount() < (PRN_RING_SIZE / 4)) {
        Serial.write(0x11);        // XON  -> host, may send
        xoffSent = false;
    }
#endif
}

// ---------------------------------------------------------------------------
static void reportPause(CentronicsPrinter::Status s) {
    digitalWrite(PIN_LED, (millis() / 120) & 1);   // blink
#if PRN_XONXOFF
    if (!xoffSent) { Serial.write(0x13); xoffSent = true; }
#endif
#if defined(PRN_DEBUG)
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    Serial.print(F("[printer] paused: "));
    switch (s) {
        case CentronicsPrinter::Status::Offline:     Serial.println(F("OFFLINE / powered off")); break;
        case CentronicsPrinter::Status::PaperOut:    Serial.println(F("OUT OF PAPER"));          break;
        case CentronicsPrinter::Status::Fault:       Serial.println(F("ERROR (/ERROR low)"));    break;
        case CentronicsPrinter::Status::BusyTimeout: Serial.println(F("BUSY stuck"));            break;
        default: Serial.println(); break;
    }
#else
    (void) s;
#endif
}

// ---------------------------------------------------------------------------
static void pumpRingIntoPrinter() {
    // short bursts so we don't hold up loop() for too long
    for (uint8_t n = 0; n < 64 && ringCount() > 0; n++) {
        const CentronicsPrinter::Status s = printer.writeByte(ringPeek());
        if (s == CentronicsPrinter::Status::Ok) {
            ringPop();
        } else {
            reportPause(s);
            return;                 // try again next loop
        }
    }
    digitalWrite(PIN_LED, ringCount() ? HIGH : LOW);
}

// ===========================================================================
//  Public interface (print_queue.h) used by the web server (src/web_print).
//  Runs on core 1, same context as loop(): no contention with the driver.
// ===========================================================================
size_t printerEnqueue(const uint8_t *data, size_t len) {
    size_t i = 0;
    for (; i < len; i++) {
        const uint8_t c = data[i];
#if PRN_TEXT_MODE
        if (c == '\r') {
            if (ringFree() < 2) break;
            ringPush('\r'); ringPush('\n');
            sawCR = true;
            continue;
        }
        if (c == '\n') {
            if (sawCR) { sawCR = false; continue; }   // swallow the \n of a CRLF
            if (ringFree() < 2) break;
            ringPush('\r'); ringPush('\n');
            continue;
        }
        sawCR = false;
        if (ringFree() < 1) break;
        ringPush(c);
#else
        if (ringFree() < 1) break;
        ringPush(c);
#endif
    }
    return i;
}

size_t printerEnqueueRaw(const uint8_t *data, size_t len) {
    size_t i = 0;
    for (; i < len && ringFree() >= 1; i++)
        ringPush(data[i]);
    return i;
}

void printerServiceOnce() {
    updateFlowControl();
    pumpRingIntoPrinter();
}

bool   printerQueueEmpty() { return ringCount() == 0; }
size_t printerQueueFree()  { return ringFree(); }

const char *printerBlockedReason() {
#if !PRN_IGNORE_STATUS
    if (!printer.online())  return "offline";
    if (printer.paperOut()) return "out of paper";
    if (printer.fault())    return "error";
#endif
    return nullptr;
}

// Publishes to MQTT (via btc_relay) whenever the state changes:
//   ready | printing | out of paper | offline | error
static void pollPrinterMqtt() {
    static uint32_t last = 0, lastActivity = 0;
    static size_t   lastRing = (size_t)-1;
    if (millis() - last < 400) return;
    last = millis();

    const size_t rc = ringCount();
    if (rc != lastRing) { lastActivity = millis(); lastRing = rc; }

    const char *blk = printerBlockedReason();
    const char *state = blk ? blk
                      : (rc > 0 || millis() - lastActivity < 3000) ? "printing"
                      : "ready";
    btcRelayPublishPrinter(state);
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.setRxBufferSize(PRN_RX_BUFFER);   // must come BEFORE begin()
    Serial.begin(SERIAL_BAUD);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    printer.begin();
#if PRN_AUTO_RESET_ON_BOOT
    printer.hardwareReset();
    printer.sendReset();                      // ESC @
#endif

#if !defined(PRN_NO_BANNER)
    Serial.println();
    Serial.println(F("=== ESP32 <-> EPSON LX-810L : serial -> parallel bridge ==="));
    Serial.print  (F("baud="));  Serial.print(SERIAL_BAUD);
    Serial.print  (F("  mode=")); Serial.println(PRN_TEXT_MODE ? F("TEXT (CRLF auto)") : F("RAW"));
    Serial.print  (F("online=")); Serial.print(printer.online() ? F("yes") : F("NO"));
    Serial.print  (F("  paper=")); Serial.print(printer.paperOut() ? F("OUT") : F("ok"));
    Serial.print  (F("  error="));  Serial.println(printer.fault() ? F("YES") : F("no"));
    Serial.println(F("READY. Send text to print."));
#endif

    // HTTP server on the ESP32 itself: a page with a form + REST endpoint
    // /print. Brings up WiFi STA (if nobody did yet) before btc_relay.
    webPrintBegin();

    // JetDirect network printer (port 9100) + mDNS advert "EPSON LX-810L"
    rawPrintBegin();

    // optional core-0 task: BTC price relay over ESP-NOW (config.h)
    btcRelayBegin();
}

void loop() {
    pumpSerialIntoRing();
    updateFlowControl();
    pumpRingIntoPrinter();
    webPrintLoop();
    rawPrintLoop();
    pollPrinterMqtt();
}
