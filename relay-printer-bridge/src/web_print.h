// ============================================================================
//  web_print  -  servidor HTTP no proprio ESP32 para enviar texto a impressora.
//
//    GET  /         pagina HTML com <textarea> + botao "Imprimir"
//    POST /print     enfileira o texto recebido (campo "texto" do form OU
//                    o corpo cru, com Content-Type: text/plain) para impressao
//    GET  /status    JSON: estado da impressora + ocupacao da fila
//
//  Roda no core 1 (dentro do loop(), via webPrintLoop()); nao concorre com o
//  driver Centronics. Reaproveita o WiFi do btc_relay quando ele esta ligado.
//  Liga/desliga e configura em include/config.h (WEBPRINT_*).
// ============================================================================
#pragma once

void webPrintBegin();  // sobe WiFi STA (se preciso) + servidor HTTP. no-op se WEBPRINT_ENABLE == 0
void webPrintLoop();    // chame no loop(). no-op se desligado
