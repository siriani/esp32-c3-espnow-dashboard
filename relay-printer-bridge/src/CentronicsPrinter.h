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
        Offline,      // SELECT baixo  -> impressora desligada ou off-line
        PaperOut,     // PE alto       -> sem papel
        Fault,        // /ERROR baixo  -> falha generica
        BusyTimeout   // BUSY nao liberou dentro do timeout
    };

    // Configura pinos, coloca as linhas em estado inativo e habilita o TXS0108E.
    void begin();

    // Pulso em /INIT (reset por hardware da impressora).
    void hardwareReset();

    // "ESC @" -> reset logico do interpretador ESC/P.
    void sendReset();

    // Leituras de status (ja tratam a logica ativo-baixo/ativo-alto).
    bool online()   const { return digitalRead(PIN_SELECT) == HIGH; }
    bool paperOut() const { return digitalRead(PIN_PE)     == HIGH; }
    bool fault()    const { return digitalRead(PIN_ERROR)  == LOW;  }
    bool busy()     const { return digitalRead(PIN_BUSY)   == HIGH; }

    // Verifica se da pra enviar agora; opcionalmente espera ate timeoutMs.
    Status poll(uint32_t timeoutMs = 0);

    // Envia 1 byte com o handshake completo. Retorna Ok ou o motivo da falha.
    Status writeByte(uint8_t b);

    Status lastStatus() const { return _last; }

    // Rotina de teste de bancada: varre D0..D7 e pulsa /STROBE devagar,
    // imprimindo o estado das linhas de status. Nao imprime nada "de verdade".
    void diagnostics(Print& log);

    // Auxiliares de teste manual (usados pelos comandos @Dn / @ST).
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
