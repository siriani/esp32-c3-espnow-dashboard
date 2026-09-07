// ============================================================================
//  raw_print.cpp  -  see raw_print.h
// ============================================================================
#include "config.h"

#ifndef RAWPRINT_ENABLE
#define RAWPRINT_ENABLE 0
#endif

#if RAWPRINT_ENABLE

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "raw_print.h"
#include "print_queue.h"

#ifndef RAWPRINT_PORT
#define RAWPRINT_PORT 9100
#endif
#ifndef RAWPRINT_MODEL
#define RAWPRINT_MODEL "EPSON LX-810L"
#endif
#ifndef RAWPRINT_IDLE_TIMEOUT_MS
#define RAWPRINT_IDLE_TIMEOUT_MS 15000UL   // no bytes -> close the connection
#endif
// 0 = normalize line endings (good with the macOS "Generic Text-Only"
//     driver, which sends only LF -- the LX-810L needs CR+LF).
// 1 = raw passthrough (use with a 9-pin ESC/P / Gutenprint driver).
#ifndef RAWPRINT_RAW
#define RAWPRINT_RAW 0
#endif
#ifndef RAWPRINT_DEBUG
#define RAWPRINT_DEBUG 1
#endif

#if RAWPRINT_DEBUG
#define RPLOG(...) Serial.printf("[9100] " __VA_ARGS__)
#else
#define RPLOG(...)
#endif

static WiFiServer server(RAWPRINT_PORT);
static WiFiClient client;
static bool g_up = false;       // server started
static bool g_announced = false; // mDNS service registered
static uint32_t g_lastByte = 0;
static uint32_t g_jobBytes = 0;

static void announce()
{
    if (g_announced || WiFi.status() != WL_CONNECTED)
        return;
    g_announced = true;
    // MDNS.begin() was already called by web_print; addService is cumulative.
    // _pdl-datastream._tcp = raw socket (JetDirect). The TXT records make macOS
    // show "EPSON LX-810L" and know the accepted formats.
    MDNS.addService("pdl-datastream", "tcp", RAWPRINT_PORT);
    MDNS.addServiceTxt("pdl-datastream", "tcp", "ty", RAWPRINT_MODEL);
    MDNS.addServiceTxt("pdl-datastream", "tcp", "product", "(" RAWPRINT_MODEL ")");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "pdl",
                       "application/vnd.epson.escp,application/octet-stream,text/plain");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "note", "ESP32 -> Centronics");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "Transparent", "T");
    MDNS.addServiceTxt("pdl-datastream", "tcp", "Binary", "T");
    RPLOG("mDNS: %s on _pdl-datastream._tcp port %d\n", RAWPRINT_MODEL, RAWPRINT_PORT);
}

void rawPrintBegin()
{
    server.begin();
    server.setNoDelay(true);
    g_up = true;
    RPLOG("waiting for jobs on port %d\n", RAWPRINT_PORT);
    if (WiFi.status() == WL_CONNECTED)
        announce();
}

void rawPrintLoop()
{
    if (!g_up)
        return;
    if (WiFi.status() != WL_CONNECTED)
        return;
    if (!g_announced)
        announce();

    // one client at a time; politely refuse the rest
    if (!client || !client.connected())
    {
        WiFiClient c = server.available();
        if (c)
        {
            if (client && client.connected())
            {
                c.stop(); // a job is already in progress
            }
            else
            {
                client = c;
                client.setNoDelay(true);
                g_lastByte = millis();
                g_jobBytes = 0;
                RPLOG("job from %s\n", client.remoteIP().toString().c_str());
            }
        }
    }

    if (client && client.connected())
    {
        uint8_t buf[256];
        int avail = client.available();
        if (avail > 0)
        {
            int n = client.read(buf, avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail);
            int off = 0;
            while (off < n)
            {
#if RAWPRINT_RAW
                off += printerEnqueueRaw(buf + off, n - off);
#else
                off += printerEnqueue(buf + off, n - off); // normalizes CR/LF
#endif
                printerServiceOnce();
                if (off < n)
                    delay(2); // queue full: let it drain
            }
            g_jobBytes += n;
            g_lastByte = millis();
        }
        else if (millis() - g_lastByte > RAWPRINT_IDLE_TIMEOUT_MS)
        {
            RPLOG("end of job: %lu bytes\n", (unsigned long)g_jobBytes);
            client.stop();
        }
    }
}

#else  // RAWPRINT_ENABLE == 0
void rawPrintBegin() {}
void rawPrintLoop() {}
#endif
