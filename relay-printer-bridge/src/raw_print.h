// ============================================================================
//  raw_print  -  "JetDirect" network printer (raw socket, port 9100).
//
//  macOS/Windows add the printer by IP ("HP Jetdirect - Socket") and send
//  the job here; bytes go THROUGH to the LX-810L's parallel port unchanged
//  (the printer interprets ESC/P). Cmd+P from any app.
//
//  Also advertised over mDNS/Bonjour as "EPSON LX-810L"
//  (_pdl-datastream._tcp) so it shows up in "Add Printer".
//
//  Runs on core 1 (loop()). Config: include/config.h (RAWPRINT_*).
// ============================================================================
#pragma once

void rawPrintBegin();
void rawPrintLoop();
