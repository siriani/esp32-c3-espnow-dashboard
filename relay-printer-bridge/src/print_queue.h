// ============================================================================
//  print_queue.h  -  the interface between the byte PRODUCERS (USB serial,
//  web server, port 9100) and the ring buffer that loop() drains into the
//  printer.
//
//  Implemented in src/main.cpp. Everything runs on core 1 (loop() + HTTP
//  handlers), so there is no contention with the Centronics driver.
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

// Enqueue up to 'len' bytes for printing. Applies the SAME end-of-line
// normalization as the serial bridge (TEXT MODE: CR, LF and CRLF -> CRLF).
// Returns how many bytes fit RIGHT NOW (may be < len if the queue filled:
// call printerServiceOnce() to drain, then retry with the rest).
size_t printerEnqueue(const uint8_t *data, size_t len);

// Like printerEnqueue but WITHOUT end-of-line normalization -- raw passthrough.
// Use for streams the host already formatted (port-9100 network printer,
// macOS/Windows ESC/P driver).
size_t printerEnqueueRaw(const uint8_t *data, size_t len);

// Drain one burst of the queue to the printer (the same thing loop() does).
// Safe to call from inside an HTTP handler.
void printerServiceOnce();

// true when the internal queue is empty. Does NOT guarantee the printer has
// finished printing its own internal buffer.
bool printerQueueEmpty();

// free space in the queue, in bytes.
size_t printerQueueFree();

// nullptr if it can print now; otherwise a short phrase with the reason
// (offline / out of paper / error). Always nullptr with -D PRN_IGNORE_STATUS=1.
const char *printerBlockedReason();
