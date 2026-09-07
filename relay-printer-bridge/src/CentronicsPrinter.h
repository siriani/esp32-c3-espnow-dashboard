// ============================================================================
//  CentronicsPrinter  -  driver for the Centronics / IEEE-1284 parallel
//  interface (classic byte-compatibility mode, as used by the Epson
//  LX-810L, ESC/P).
//
//  Per-byte handshake:
//    1) wait for BUSY low  (and check on-line / paper / error)
//    2) drive D0..D7 onto the bus
//    3) low pulse on /STROBE  (>= 0.5 us)
//    4) the printer raises BUSY, processes, then releases it (pulse on /ACK)
// ============================================================================
#pragma once
#include <Arduino.h>
#include "config.h"

class CentronicsPrinter {
public:
    enum class Status : uint8_t {
        Ok = 0,
        Offline,      // SELECT low   -> printer powered off or offline
        PaperOut,     // PE high      -> out of paper
        Fault,        // /ERROR low   -> generic fault
        BusyTimeout   // BUSY didn't clear within the timeout
    };

    // Configure pins, park the lines idle and enable the TXS0108E.
    void begin();

    // Pulse on /INIT (hardware reset of the printer).
    void hardwareReset();

    // "ESC @" -> logical reset of the ESC/P interpreter.
    void sendReset();

    // Status reads (active-low/active-high logic already handled).
    bool online()   const { return digitalRead(PIN_SELECT) == HIGH; }
    bool paperOut() const { return digitalRead(PIN_PE)     == HIGH; }
    bool fault()    const { return digitalRead(PIN_ERROR)  == LOW;  }
    bool busy()     const { return digitalRead(PIN_BUSY)   == HIGH; }

    // Check whether we can send right now; optionally wait up to timeoutMs.
    Status poll(uint32_t timeoutMs = 0);

    // Send 1 byte with the full handshake. Returns Ok or the failure reason.
    Status writeByte(uint8_t b);

    Status lastStatus() const { return _last; }

    // Bench-test routine: sweeps D0..D7 and pulses /STROBE slowly,
    // printing the state of the status lines. Prints nothing "for real".
    void diagnostics(Print& log);

    // Manual-test helpers (used by the @Dn / @ST commands).
    void dbgSetData(uint8_t mask) { setData(mask); }
    void dbgStrobe(uint16_t lowMs) {
        digitalWrite(PIN_STROBE, LOW); delay(lowMs); digitalWrite(PIN_STROBE, HIGH);
    }

    uint32_t busyTimeoutMs = PRN_BUSY_TIMEOUT_MS;

private:
    void setData(uint8_t b);

    Status _last = Status::Ok;
    static const uint8_t _data[8];
};
