#ifndef PARAM_PERSIST_H
#define PARAM_PERSIST_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "login_save.h"

// Dimensioni massime standard per i parametri
#define MAX_IP_LEN   16  // Es: 255.255.255.255
#define MAX_HOST_LEN 64  // Es: www.google.com

typedef struct {
    wifi_login_store_t wifi_logins;
    char ping_ip[MAX_IP_LEN];       
    char ping_host[MAX_HOST_LEN];   
} app_config_t;

// Funzioni pubbliche
esp_err_t config_nvs_init(void);
esp_err_t config_load(app_config_t *config);
esp_err_t config_save(const app_config_t *config);

#endif // PARAM_PERSIST_H
