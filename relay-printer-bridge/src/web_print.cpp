// ============================================================================
//  web_print.cpp  -  see web_print.h
// ============================================================================
#include "config.h"

#ifndef WEBPRINT_ENABLE
#define WEBPRINT_ENABLE 0
#endif

#if WEBPRINT_ENABLE

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "web_print.h"
#include "print_queue.h"

// ---- defaults (override in include/config.h) -----------------------------
#ifndef WEBPRINT_WIFI_SSID
#define WEBPRINT_WIFI_SSID BTC_WIFI_SSID
#endif
#ifndef WEBPRINT_WIFI_PASS
#define WEBPRINT_WIFI_PASS BTC_WIFI_PASS
#endif
#ifndef WEBPRINT_PORT
#define WEBPRINT_PORT 80
#endif
#ifndef WEBPRINT_HOSTNAME
#define WEBPRINT_HOSTNAME "dotmatrix"
#endif
#ifndef WEBPRINT_MAX_BODY
#define WEBPRINT_MAX_BODY 16384
#endif
#ifndef WEBPRINT_FEED_TIMEOUT_MS
#define WEBPRINT_FEED_TIMEOUT_MS 20000UL
#endif
#ifndef WEBPRINT_WIFI_TIMEOUT_MS
#define WEBPRINT_WIFI_TIMEOUT_MS 15000UL
#endif
#ifndef WEBPRINT_TOKEN
#define WEBPRINT_TOKEN ""
#endif
#ifndef WEBPRINT_DEBUG
#define WEBPRINT_DEBUG 1
#endif

#if WEBPRINT_DEBUG
#define WPLOG(...) Serial.printf("[web] " __VA_ARGS__)
#else
#define WPLOG(...)
#endif

static WebServer server(WEBPRINT_PORT);
static bool g_announced = false;   // already logged the IP / registered mDNS

// ---- HTML page ----------------------------------------------------------
// No external resources (the ESP can't serve a CDN and the network is local).
// The %SENT% / %HOST% / %TOKEN% markers are substituted on every request.
static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LX-810L printer</title>
<style>
 :root{color-scheme:light dark}
 body{font-family:system-ui,-apple-system,sans-serif;max-width:42rem;margin:2rem auto;padding:0 1rem;line-height:1.4}
 h1{font-size:1.2rem;margin:.2rem 0 1rem}
 textarea{width:100%;min-height:15rem;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:1rem;padding:.6rem;box-sizing:border-box}
 .row{margin-top:.8rem;display:flex;gap:1rem 1.4rem;align-items:center;flex-wrap:wrap}
 button{font-size:1rem;padding:.55rem 1.6rem;cursor:pointer;border-radius:.4rem}
 label{font-size:.9rem}
 .badge{padding:.15rem .55rem;border-radius:.4rem;background:#8883;font-size:.85rem}
 .ok{color:#0a7a0a}.bad{color:#c62222}
 .sent{background:#12a45422;border:1px solid #12a45455;padding:.55rem .8rem;border-radius:.4rem;margin-bottom:1rem}
 footer{margin-top:1.6rem;font-size:.8rem;opacity:.75}
 code{background:#8882;padding:.05rem .3rem;border-radius:.25rem;word-break:break-all}
</style>
<h1>&#128424;&#65039; EPSON LX-810L printer</h1>
%SENT%
<form method="post" action="print">
 <textarea name="text" autofocus placeholder="Type what you want to print..."></textarea>
 <input type="hidden" name="token" value="%TOKEN%">
 <div class="row">
  <button type="submit">Print</button>
  <label><input type="checkbox" name="ff" value="1"> form feed at the end</label>
  <span class="badge" id="st">state: &mdash;</span>
 </div>
</form>
<footer>
 REST: <code>curl -sS --data-binary @file.txt -H "Content-Type: text/plain" http://%HOST%/print</code>
 <div id="q" style="margin-top:.4rem"></div>
</footer>
<script>
var TK='%TOKEN%';
function q(u){return TK?u+'?token='+encodeURIComponent(TK):u}
async function poll(){
 try{
  var r=await fetch(q('status'),{cache:'no-store'});var j=await r.json();
  var s=document.getElementById('st');
  s.textContent='state: '+j.state;
  s.className='badge '+(j.ready?'ok':'bad');
  document.getElementById('q').textContent='queue: '+j.queue_free+' of '+j.queue_total+' bytes free';
 }catch(e){}
}
poll();setInterval(poll,3000);
</script>
)HTML";

// ---- helpers ------------------------------------------------------------
static bool tokenRequired()
{
    const char *t = WEBPRINT_TOKEN;
    return t && t[0];
}

static bool authOk()
{
    if (!tokenRequired())
        return true;
    if (server.hasArg("token") && server.arg("token") == WEBPRINT_TOKEN)
        return true;
    if (server.hasHeader("X-Auth-Token") && server.header("X-Auth-Token") == WEBPRINT_TOKEN)
        return true;
    return false;
}

static void announce()
{
    if (g_announced || WiFi.status() != WL_CONNECTED)
        return;
    g_announced = true;

    const char *host = WEBPRINT_HOSTNAME;
    if (host && host[0] && MDNS.begin(host))
        MDNS.addService("http", "tcp", WEBPRINT_PORT);

    WPLOG("ready -> http://%s/", WiFi.localIP().toString().c_str());
    if (host && host[0])
        Serial.printf("   or  http://%s.local/", host);
    Serial.println();
}

// ---- handlers --------------------------------------------------------------
static void handleRoot()
{
    String sent;
    if (server.hasArg("sent"))
        sent = "<div class=\"sent\">&#10004; " + String(server.arg("sent").toInt()) +
               " bytes sent to the printer.</div>";

    String page = FPSTR(PAGE);
    page.replace("%SENT%", sent);
    page.replace("%HOST%", WiFi.localIP().toString());
    page.replace("%TOKEN%", tokenRequired() ? String(WEBPRINT_TOKEN) : String());

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", page);
}

static void handleStatus()
{
    const char *blk = printerBlockedReason();
    String j = "{";
    j += "\"ready\":";
    j += (blk ? "false" : "true");
    j += ",\"state\":\"";
    j += (blk ? blk : "ready");
    j += "\",\"queue_free\":";
    j += printerQueueFree();
    j += ",\"queue_total\":";
    j += (uint32_t)(PRN_RING_SIZE - 1);
    j += ",\"queue_empty\":";
    j += (printerQueueEmpty() ? "true" : "false");
    j += ",\"ip\":\"";
    j += WiFi.localIP().toString();
    j += "\"}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

static void handlePrint()
{
    if (!authOk())
    {
        server.send(401, "text/plain; charset=utf-8", "invalid token\n");
        return;
    }

    // text source: form field "text", or the raw body ("plain", sent with
    // Content-Type: text/plain)
    String body;
    bool fromForm = false;
    if (server.hasArg("text"))
    {
        body = server.arg("text");
        fromForm = true;
    }
    else if (server.hasArg("plain"))
    {
        body = server.arg("plain");
    }
    else
    {
        server.send(400, "text/plain; charset=utf-8",
                    "nothing to print: use the 'text' field (form) or "
                    "send the body with Content-Type: text/plain\n");
        return;
    }

    if (fromForm && body.length() == 0)
    {
        server.sendHeader("Location", "/");
        server.send(303, "text/plain", "");
        return;
    }
    if (body.length() == 0)
    {
        server.send(400, "text/plain; charset=utf-8", "empty body\n");
        return;
    }
    if (body.length() > WEBPRINT_MAX_BODY)
    {
        server.send(413, "text/plain; charset=utf-8",
                    String("text too large (max ") + WEBPRINT_MAX_BODY + " bytes)\n");
        return;
    }

    const char *blk = printerBlockedReason();
    if (blk)
    {
        server.send(503, "text/plain; charset=utf-8",
                    String("printer unavailable: ") + blk + "\n");
        return;
    }

    if (fromForm && server.hasArg("ff"))
        body += '\f'; // form feed: advance the page at the end of the job

    // push into the queue while draining it at the same time; give up if it stalls
    const uint8_t *p = (const uint8_t *)body.c_str();
    const size_t total = body.length();
    size_t done = 0;
    uint32_t progress = millis();
    while (done < total)
    {
        size_t n = printerEnqueue(p + done, total - done);
        done += n;
        if (n)
            progress = millis();
        printerServiceOnce();
        if (done < total)
        {
            if (millis() - progress > WEBPRINT_FEED_TIMEOUT_MS)
                break;
            delay(2);
        }
    }

    WPLOG("POST /print: %u of %u bytes queued%s\n",
          (unsigned)done, (unsigned)total, done < total ? " (TIMEOUT)" : "");

    if (done < total)
    {
        server.send(504, "text/plain; charset=utf-8",
                    String("timed out: ") + done + " of " + total + " bytes queued\n");
        return;
    }
    if (fromForm)
    {
        server.sendHeader("Location", String("/?sent=") + done);
        server.send(303, "text/plain", "");
    }
    else
    {
        server.send(200, "text/plain; charset=utf-8",
                    String("ok: ") + done + " bytes queued\n");
    }
}

// ---- API ----------------------------------------------------------------
void webPrintBegin()
{
    WiFi.persistent(false);

    const char *host = WEBPRINT_HOSTNAME;
    if (host && host[0])
        WiFi.setHostname(host);

    // only start associating if nobody (e.g. btc_relay) already did
    if (WiFi.status() != WL_CONNECTED)
    {
        WiFi.mode(WIFI_STA);
        WiFi.begin(WEBPRINT_WIFI_SSID, WEBPRINT_WIFI_PASS);
        WPLOG("WiFi \"%s\" ...\n", WEBPRINT_WIFI_SSID);
    }
    WiFi.setSleep(false);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WEBPRINT_WIFI_TIMEOUT_MS)
        delay(150);

    const char *hdrs[] = {"X-Auth-Token"};
    server.collectHeaders(hdrs, 1);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/print", HTTP_POST, handlePrint);
    server.on("/print", HTTP_GET, []()
              { server.sendHeader("Location", "/"); server.send(303, "text/plain", ""); });
    server.onNotFound([]()
                      { server.send(404, "text/plain; charset=utf-8", "not found\n"); });
    server.begin();

    if (WiFi.status() == WL_CONNECTED)
        announce();
    else
        WPLOG("WiFi not associated yet; the server comes up as soon as it connects\n");
}

void webPrintLoop()
{
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!g_announced)
            announce();
        server.handleClient();
    }
}

#else  // WEBPRINT_ENABLE == 0
void webPrintBegin() {}
void webPrintLoop() {}
#endif
