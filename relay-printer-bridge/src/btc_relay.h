// ============================================================================
//  btc_relay  -  tarefa opcional que roda no core 0, isolada da ponte da
//  impressora (que fica no core 1). Conecta no WiFi, busca o preco do BTC
//  na CoinGecko e transmite por ESP-NOW para o display ESP32-C3 (que nao
//  consegue associar WiFi por conta da antena).
//
//  Ativar/desativar e configurar em include/config.h (BTC_RELAY_*).
//  Nao toca em nada do CentronicsPrinter; so chamar btcRelayBegin() no setup.
// ============================================================================
#pragma once

void btcRelayBegin(); // no-op se BTC_RELAY_ENABLE == 0

// Publica o status da impressora em MQTT (topico "impressora/status",
// retido). Chame do core 1 (loop()) -- so guarda a string; quem publica
// e a task do relay no core 0, sem concorrencia no PubSubClient.
// Passe uma frase curta ("pronta", "imprimindo", "sem papel", ...).
// no-op se BTC_RELAY_ENABLE ou BTC_MQTT_ENABLE == 0.
void btcRelayPublishPrinter(const char *estado);
