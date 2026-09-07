// ============================================================================
//  print_queue.h  -  ponte entre os PRODUTORES de bytes (Serial USB e servidor
//  web) e a fila circular que o loop() escoa para a impressora.
//
//  Implementado em src/main.cpp. Tudo roda no core 1 (loop() + handlers HTTP),
//  entao nao ha concorrencia com o driver Centronics.
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
