// ============================================================================
//  web_print  -  HTTP server on the ESP32 to send text to the printer.
//
//    GET  /         HTML page with a <textarea> + "Print" button
//    POST /print     enqueue the text (form field "texto" OR the raw body,
//                    with Content-Type: text/plain) for printing
//    GET  /status    JSON: printer state + queue occupancy
//
//  Runs on core 1 (inside loop(), via webPrintLoop()); does not race the
//  Centronics driver. Reuses btc_relay's WiFi when it is enabled.
//  Enable / configure in include/config.h (WEBPRINT_*).
// ============================================================================
#pragma once

void webPrintBegin();  // sobe WiFi STA (se preciso) + servidor HTTP. no-op se WEBPRINT_ENABLE == 0
void webPrintLoop();    // chame no loop(). no-op se desligado
