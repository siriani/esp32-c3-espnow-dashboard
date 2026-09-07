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

// Enfileira ate 'len' bytes para impressao. Aplica a MESMA normalizacao de
// fim de linha da ponte serial (MODO TEXTO: CR, LF e CRLF -> CRLF).
// Retorna quantos bytes couberam AGORA (pode ser < len se a fila encheu:
// chame printerServiceOnce() para escoar e repita com o restante).
size_t printerEnqueue(const uint8_t *data, size_t len);

// Igual a printerEnqueue mas SEM normalizar fim de linha -- passthrough cru.
// Use para fluxos que o host ja formatou (impressora de rede porta 9100,
// driver ESC/P do macOS/Windows).
size_t printerEnqueueRaw(const uint8_t *data, size_t len);

// Escoa uma rajada da fila para a impressora (o mesmo que o loop() faz).
// Seguro chamar de dentro de um handler HTTP.
void printerServiceOnce();

// true quando a fila interna esta vazia. NAO garante que a impressora ja
// terminou de imprimir o proprio buffer interno.
bool printerQueueEmpty();

// Espaco livre na fila, em bytes.
size_t printerQueueFree();

// nullptr se da pra imprimir agora; senao uma frase curta com o motivo
// (off-line / sem papel / erro). Sempre nullptr com -D PRN_IGNORE_STATUS=1.
const char *printerBlockedReason();
