#include "CentronicsPrinter.h"

const uint8_t CentronicsPrinter::_data[8] = {
    PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7
};

void CentronicsPrinter::begin() {
    // Linhas de dados: saidas em 0.
    for (uint8_t i = 0; i < 8; i++) {
        pinMode(_data[i], OUTPUT);
        digitalWrite(_data[i], LOW);
    }

    // Controles ativos-baixo: comecam INATIVOS (nivel alto).
    pinMode(PIN_STROBE, OUTPUT);
    digitalWrite(PIN_STROBE, HIGH);
    pinMode(PIN_INIT, OUTPUT);
    digitalWrite(PIN_INIT, HIGH);

    // /AUTOFEED = ALTO (sem auto line feed) ; /SELECT-IN = BAIXO (seleciona a impressora)
    pinMode(PIN_AUTOFEED, OUTPUT);
    digitalWrite(PIN_AUTOFEED, HIGH);
    pinMode(PIN_SELIN, OUTPUT);
    digitalWrite(PIN_SELIN, LOW);

    // Status vindos da impressora (entram por ~1k em serie ou divisor).
    pinMode(PIN_BUSY,   INPUT);
    pinMode(PIN_PE,     INPUT);
    pinMode(PIN_ERROR,  INPUT);
    pinMode(PIN_SELECT, INPUT);
    pinMode(PIN_ACK,    INPUT);

    delay(5);
}

void CentronicsPrinter::hardwareReset() {
    digitalWrite(PIN_INIT, LOW);
    delayMicroseconds(100);           // spec: >= 50 us
    digitalWrite(PIN_INIT, HIGH);
    delay(50);                        // deixa a impressora reinicializar
}

void CentronicsPrinter::sendReset() {
    writeByte(0x1B);                  // ESC
    writeByte('@');
}

CentronicsPrinter::Status CentronicsPrinter::poll(uint32_t timeoutMs) {
    const uint32_t start = millis();
    for (;;) {
#if !PRN_IGNORE_STATUS
        if (!online())  { _last = Status::Offline;  return _last; }
        if (paperOut()) { _last = Status::PaperOut; return _last; }
        if (fault())    { _last = Status::Fault;    return _last; }
#endif
        if (!busy())    { _last = Status::Ok;       return _last; }
        if (millis() - start >= timeoutMs) { _last = Status::BusyTimeout; return _last; }
        delayMicroseconds(50);
    }
}

void CentronicsPrinter::setData(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++)
        digitalWrite(_data[i], (b >> i) & 0x01);
}

void CentronicsPrinter::diagnostics(Print& log) {
    log.println();
    log.println(F("=================  DIAG  ================="));
    log.print(F("SELECT/on-line = ")); log.print(online()   ? F("ALTO (ok)") : F("baixo"));
    log.print(F("   BUSY = "));         log.print(busy()     ? F("ALTO")      : F("baixo (ok)"));
    log.print(F("   PE/papel = "));     log.print(paperOut() ? F("FALTA")     : F("ok"));
    log.print(F("   /ERROR = "));       log.println(fault()  ? F("BAIXO (falha)") : F("alto (ok)"));

    log.println(F("[1] cada linha de dados ALTA por 1 s - meca no DB25 (deve dar ~5 V):"));
    for (uint8_t i = 0; i < 8; i++) {
        setData((uint8_t)(1u << i));
        log.print(F("    D")); log.print(i);
        log.print(F("  ->  DB25 pino ")); log.print(i + 2);
        log.println(F("   (os outros ~0 V)"));
        delay(1000);
    }
    setData(0);

    log.println(F("[2] 6 pulsos LENTOS de /STROBE (dados=0x55), olhando o BUSY:"));
    setData(0x55);
    for (uint8_t k = 0; k < 6; k++) {
        const bool b0 = busy();
        digitalWrite(PIN_STROBE, LOW);
        delay(3);
        digitalWrite(PIN_STROBE, HIGH);
        delayMicroseconds(200);
        const bool b1 = busy();
        delay(15);
        const bool b2 = busy();
        log.print(F("    pulso ")); log.print(k + 1);
        log.print(F(":  BUSY antes=")); log.print(b0);
        log.print(F("  logo apos=")); log.print(b1);
        log.print(F("  +15ms=")); log.println(b2);
        delay(400);
    }
    setData(0);
    log.println(F("Se o BUSY mexe nos pulsos -> a impressora ESTA recebendo o strobe."));
    log.println(F("Se nunca mexe -> /STROBE nao chega, ou /SELECT-IN (DB25 17) nao esta no GND."));
    log.println(F("=========================================")); log.println();
}

CentronicsPrinter::Status CentronicsPrinter::writeByte(uint8_t b) {
    // 1) espera a impressora ficar pronta
    const Status s = poll(busyTimeoutMs);
    if (s != Status::Ok) return s;

    // 2) coloca o dado no barramento
    setData(b);
    delayMicroseconds(2);            // data setup (spec: >= 0,5 us)

    // 3) pulso de /STROBE (ativo baixo)  (spec: >= 0,5 us)
    digitalWrite(PIN_STROBE, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_STROBE, HIGH);

    // 4) hold dos dados apos o strobe
    delayMicroseconds(2);

    // 5) aguarda a impressora reconhecer (subir BUSY), com teto curto:
    //    bytes rapidos podem nem chegar a levantar BUSY de forma visivel.
    const uint32_t t0 = micros();
    while (!busy()) {
        if (micros() - t0 > 2000) break;   // 2 ms
    }

    _last = Status::Ok;
    return _last;
}
