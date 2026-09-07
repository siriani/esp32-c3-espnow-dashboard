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

void btcRelayBegin(); // no-op if BTC_RELAY_ENABLE == 0

// Publish the printer status to MQTT (topic "printer/status", retained).
// Call from core 1 (loop()) -- it only stores the string; the actual
// publish happens on the relay task on core 0, so PubSubClient is never
// touched from two cores. Pass a short phrase ("ready", "printing",
// "out of paper", ...). no-op if BTC_RELAY_ENABLE or BTC_MQTT_ENABLE == 0.
void btcRelayPublishPrinter(const char *state);
