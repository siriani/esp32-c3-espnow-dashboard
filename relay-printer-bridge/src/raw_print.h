// ============================================================================
//  raw_print  -  impressora de rede "JetDirect" (socket cru, porta 9100).
//
//  O macOS/Windows adiciona a impressora por IP (protocolo "HP Jetdirect -
//  Socket") e manda o job pra ca; os bytes vao CRUS pra porta paralela da
//  LX-810L (a impressora interpreta ESC/P). Command+P de qualquer app.
//
//  Tambem anuncia por mDNS/Bonjour como "EPSON LX-810L" (_pdl-datastream._tcp)
//  pra aparecer no "Adicionar impressora".
//
//  Roda no core 1 (loop()). Config: include/config.h (RAWPRINT_*).
// ============================================================================
#pragma once

void rawPrintBegin();
void rawPrintLoop();
