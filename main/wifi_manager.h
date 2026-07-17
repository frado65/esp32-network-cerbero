#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "param_persist.h"

/* Configura il pulsante di recupero e avvia Wi-Fi in modalità AP o Station. */
esp_err_t wifi_manager_start(const app_config_t *config);

/* Blocca il task chiamante finché la Station non riceve un indirizzo IP. */
void wifi_manager_wait_connected(TickType_t timeout_ticks);

#endif // WIFI_MANAGER_H
