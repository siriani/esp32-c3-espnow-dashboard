// ============================================================================
//  btc_relay  -  optional core-0 task, isolated from the printer bridge on
//  core 1. Joins WiFi, fetches prices/weather, runs MQTT + NTP, and
//  broadcasts everything over ESP-NOW to the ESP32-C3 display (whose own
//  WiFi can't associate with a router -- see ../docs/esp32-c3-wifi-problem.md).
//
//  Enable / configure in include/config.h (BTC_RELAY_*).
//  Touches nothing in CentronicsPrinter; just call btcRelayBegin() in setup().
// ============================================================================
#pragma once

void btcRelayBegin(); // no-op se BTC_RELAY_ENABLE == 0

// Publica o status da impressora em MQTT (topico "impressora/status",
// retido). Chame do core 1 (loop()) -- so guarda a string; quem publica
// e a task do relay no core 0, sem concorrencia no PubSubClient.
// Passe uma frase curta ("pronta", "imprimindo", "sem papel", ...).
// no-op se BTC_RELAY_ENABLE ou BTC_MQTT_ENABLE == 0.
void btcRelayPublishPrinter(const char *estado);
