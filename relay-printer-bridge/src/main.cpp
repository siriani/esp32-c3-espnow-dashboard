// ============================================================================
//  ESP32  <->  EPSON LX-810L
//  Ponte: tudo que chega na Serial (USB) e impresso na porta paralela.
//
//  - MODO TEXTO (padrao): CR, LF e CRLF viram CRLF -> texto do terminal sai
//    com quebras de linha corretas.
//  - MODO RAW (-D PRN_TEXT_MODE=0): fluxo intacto, para ESC/P binario.
//
//  LED da placa:
//    apagado  -> ocioso
//    aceso    -> imprimindo / ha dados na fila
//    piscando -> impressora pausada (off-line, sem papel, erro ou BUSY travado)
// ============================================================================
#include <Arduino.h>
#include "config.h"
#include "CentronicsPrinter.h"
#include "print_queue.h"
#include "btc_relay.h"
#include "web_print.h"
#include "raw_print.h"

static CentronicsPrinter printer;

// ---- buffer circular entre a Serial (USB) e a impressora -------------------
static uint8_t  ring[PRN_RING_SIZE];
static size_t   rHead = 0, rTail = 0;

static inline size_t  ringCount() { return (rHead - rTail) & (PRN_RING_SIZE - 1); }
static inline size_t  ringFree()  { return PRN_RING_SIZE - 1 - ringCount(); }
static inline void    ringPush(uint8_t b) { ring[rHead] = b; rHead = (rHead + 1) & (PRN_RING_SIZE - 1); }
static inline uint8_t ringPeek()  { return ring[rTail]; }
static inline void    ringPop()   { rTail = (rTail + 1) & (PRN_RING_SIZE - 1); }

static bool xoffSent = false;

#if PRN_TEXT_MODE
static bool sawCR = false;      // acabamos de empurrar um CRLF por causa de um CR
#endif

#if defined(PRN_DEBUG)
// ---- comandos de bancada (digite a linha e Enter) ----
//   @DIAG        autoteste completo (varre D0..D7, pulsa /STROBE)
//   @PINS        mostra SELECT/BUSY/PE/ERROR por 8 s (mexa o On Line da impressora)
//   @D0 .. @D7   forca so aquela linha de dados em ALTO (mede no DB25 2..9)
//   @DX          zera o barramento de dados
//   @ST          um pulso lento de /STROBE, olhando o BUSY
#include <string.h>
static char g_cmd[12];
static uint8_t g_cmdn = 0;

static void runCmd(const char* s) {
    if (!strcmp(s, "@DIAG")) { printer.diagnostics(Serial); return; }
    if (!strcmp(s, "@PINS")) {
        Serial.println(F("[PINS] 8 s - mexa o On Line / tampa / papel e observe:"));
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
        Serial.print(F("] ALTO -> meca no DB25 pino ")); Serial.print(s[2] - '0' + 2);
        Serial.println(F(" (~5 V). '@DX' zera."));
        return;
    }
    if (!strcmp(s, "@DX")) { printer.dbgSetData(0); Serial.println(F("[DX] dados = 0")); return; }
    if (!strcmp(s, "@ST")) {
        const bool a = printer.busy();
        printer.dbgStrobe(5);
        delay(15);
        const bool b = printer.busy();
        Serial.print(F("[ST] BUSY antes=")); Serial.print(a);
        Serial.print(F("  depois=")); Serial.println(b);
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
    // deixa folga de 2 bytes (MODO TEXTO pode empurrar CRLF de uma vez)
    while (Serial.available() && ringFree() > 2) {
        const uint8_t c = (uint8_t) Serial.read();
#if defined(PRN_DEBUG)
        feedCmd(c);
#endif
#if PRN_TEXT_MODE
        if (c == '\r') { ringPush('\r'); ringPush('\n'); sawCR = true;  continue; }
        if (c == '\n') { if (sawCR) { sawCR = false; continue; }        // engole o \n do CRLF
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
        Serial.write(0x13);        // XOFF -> host, pare de enviar
        xoffSent = true;
    } else if (xoffSent && ringCount() < (PRN_RING_SIZE / 4)) {
        Serial.write(0x11);        // XON  -> host, pode enviar
        xoffSent = false;
    }
#endif
}

// ---------------------------------------------------------------------------
static void reportPause(CentronicsPrinter::Status s) {
    digitalWrite(PIN_LED, (millis() / 120) & 1);   // pisca
#if PRN_XONXOFF
    if (!xoffSent) { Serial.write(0x13); xoffSent = true; }
#endif
#if defined(PRN_DEBUG)
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    Serial.print(F("[impressora] pausada: "));
    switch (s) {
        case CentronicsPrinter::Status::Offline:     Serial.println(F("OFF-LINE / desligada")); break;
        case CentronicsPrinter::Status::PaperOut:    Serial.println(F("SEM PAPEL"));            break;
        case CentronicsPrinter::Status::Fault:       Serial.println(F("ERRO (/ERROR baixo)")); break;
        case CentronicsPrinter::Status::BusyTimeout: Serial.println(F("BUSY travado"));         break;
        default: Serial.println(); break;
    }
#else
    (void) s;
#endif
}

// ---------------------------------------------------------------------------
static void pumpRingIntoPrinter() {
    // rajadas curtas para nao segurar o loop() por muito tempo
    for (uint8_t n = 0; n < 64 && ringCount() > 0; n++) {
        const CentronicsPrinter::Status s = printer.writeByte(ringPeek());
        if (s == CentronicsPrinter::Status::Ok) {
            ringPop();
        } else {
            reportPause(s);
            return;                 // tenta de novo no proximo loop
        }
    }
    digitalWrite(PIN_LED, ringCount() ? HIGH : LOW);
}

// ===========================================================================
//  Interface publica (print_queue.h) usada pelo servidor web (src/web_print).
//  Roda no core 1, no mesmo contexto do loop(): sem concorrencia com o driver.
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
            if (sawCR) { sawCR = false; continue; }   // engole o \n do CRLF
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
    if (!printer.online())  return "impressora off-line ou desligada";
    if (printer.paperOut()) return "sem papel";
    if (printer.fault())    return "erro na impressora (/ERROR)";
#endif
    return nullptr;
}

// Publica no MQTT (via btc_relay) sempre que o estado muda:
//   pronta | imprimindo | sem papel | off-line | erro
static void pollPrinterMqtt() {
    static uint32_t last = 0, lastActivity = 0;
    static size_t   lastRing = (size_t)-1;
    if (millis() - last < 400) return;
    last = millis();

    const size_t rc = ringCount();
    if (rc != lastRing) { lastActivity = millis(); lastRing = rc; }

    const char *blk = printerBlockedReason();
    const char *estado = blk ? blk
                       : (rc > 0 || millis() - lastActivity < 3000) ? "imprimindo"
                       : "pronta";
    btcRelayPublishPrinter(estado);
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.setRxBufferSize(PRN_RX_BUFFER);   // precisa vir ANTES de begin()
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
    Serial.println(F("=== ESP32 <-> EPSON LX-810L : ponte serial -> paralela ==="));
    Serial.print  (F("baud="));  Serial.print(SERIAL_BAUD);
    Serial.print  (F("  modo=")); Serial.println(PRN_TEXT_MODE ? F("TEXTO (CRLF auto)") : F("RAW"));
    Serial.print  (F("on-line=")); Serial.print(printer.online() ? F("sim") : F("NAO"));
    Serial.print  (F("  papel=")); Serial.print(printer.paperOut() ? F("FALTA") : F("ok"));
    Serial.print  (F("  erro="));  Serial.println(printer.fault() ? F("SIM") : F("nao"));
    Serial.println(F("PRONTO. Envie texto para imprimir."));
#endif

    // servidor HTTP no proprio ESP32: pagina com formulario + endpoint REST
    // /print. Sobe o WiFi STA (se ainda ninguem subiu) antes do btc_relay.
    webPrintBegin();

    // impressora de rede JetDirect (porta 9100) + anuncio mDNS "EPSON LX-810L"
    rawPrintBegin();

    // tarefa opcional no core 0: relay do preco do BTC via ESP-NOW (config.h)
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
