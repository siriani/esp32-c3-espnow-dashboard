#include "CentronicsPrinter.h"

const uint8_t CentronicsPrinter::_data[8] = {
    PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7
};

void CentronicsPrinter::begin() {
    // Data lines: outputs at 0.
    for (uint8_t i = 0; i < 8; i++) {
        pinMode(_data[i], OUTPUT);
        digitalWrite(_data[i], LOW);
    }

    // Active-low controls: start INACTIVE (high level).
    pinMode(PIN_STROBE, OUTPUT);
    digitalWrite(PIN_STROBE, HIGH);
    pinMode(PIN_INIT, OUTPUT);
    digitalWrite(PIN_INIT, HIGH);

    // /AUTOFEED = HIGH (no auto line feed) ; /SELECT-IN = LOW (selects the printer)
    pinMode(PIN_AUTOFEED, OUTPUT);
    digitalWrite(PIN_AUTOFEED, HIGH);
    pinMode(PIN_SELIN, OUTPUT);
    digitalWrite(PIN_SELIN, LOW);

    // Status lines from the printer (in through a ~1k series resistor or a divider).
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
    delay(50);                        // let the printer reinitialize
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
    log.print(F("SELECT/online = ")); log.print(online()   ? F("HIGH (ok)") : F("low"));
    log.print(F("   BUSY = "));        log.print(busy()     ? F("HIGH")      : F("low (ok)"));
    log.print(F("   PE/paper = "));    log.print(paperOut() ? F("OUT")       : F("ok"));
    log.print(F("   /ERROR = "));      log.println(fault()  ? F("LOW (fault)") : F("high (ok)"));

    log.println(F("[1] each data line HIGH for 1 s - measure on DB25 (should read ~5 V):"));
    for (uint8_t i = 0; i < 8; i++) {
        setData((uint8_t)(1u << i));
        log.print(F("    D")); log.print(i);
        log.print(F("  ->  DB25 pin ")); log.print(i + 2);
        log.println(F("   (the others ~0 V)"));
        delay(1000);
    }
    setData(0);

    log.println(F("[2] 6 SLOW /STROBE pulses (data=0x55), watching BUSY:"));
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
        log.print(F("    pulse ")); log.print(k + 1);
        log.print(F(":  BUSY before=")); log.print(b0);
        log.print(F("  right after=")); log.print(b1);
        log.print(F("  +15ms=")); log.println(b2);
        delay(400);
    }
    setData(0);
    log.println(F("If BUSY moves on the pulses -> the printer IS receiving the strobe."));
    log.println(F("If it never moves -> /STROBE isn't arriving, or /SELECT-IN (DB25 17) isn't at GND."));
    log.println(F("=========================================")); log.println();
}

CentronicsPrinter::Status CentronicsPrinter::writeByte(uint8_t b) {
    // 1) wait for the printer to be ready
    const Status s = poll(busyTimeoutMs);
    if (s != Status::Ok) return s;

    // 2) put the data on the bus
    setData(b);
    delayMicroseconds(2);            // data setup (spec: >= 0.5 us)

    // 3) /STROBE pulse (active low)  (spec: >= 0.5 us)
    digitalWrite(PIN_STROBE, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_STROBE, HIGH);

    // 4) data hold after the strobe
    delayMicroseconds(2);

    // 5) wait for the printer to acknowledge (raise BUSY), with a short cap:
    //    fast bytes may never raise BUSY visibly.
    const uint32_t t0 = micros();
    while (!busy()) {
        if (micros() - t0 > 2000) break;   // 2 ms
    }

    _last = Status::Ok;
    return _last;
}
