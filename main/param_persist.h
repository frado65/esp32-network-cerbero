#ifndef PARAM_PERSIST_H
#define PARAM_PERSIST_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Dimensioni massime standard per i parametri
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64
#define MAX_IP_LEN   16  // Es: 255.255.255.255
#define MAX_HOST_LEN 64  // Es: www.google.com

typedef struct {
    char wifi_ssid[MAX_SSID_LEN];
    char wifi_password[MAX_PASS_LEN];
    char ping_ip[MAX_IP_LEN];       
    char ping_host[MAX_HOST_LEN];   
} app_config_t;

// Funzioni pubbliche
esp_err_t config_nvs_init(void);
esp_err_t config_load(app_config_t *config);
esp_err_t config_save(const app_config_t *config);

#endif // PARAM_PERSIST_H

