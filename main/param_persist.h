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
    /* Questa struttura è salvata come singolo blob NVS: modificarne layout o
     * dimensione richiede considerare la compatibilità con dati già in Flash. */
    wifi_login_store_t wifi_logins;
    char ping_ip[MAX_IP_LEN];       
    char ping_host[MAX_HOST_LEN];   
} app_config_t;

// Funzioni pubbliche
// Prepara la partizione NVS usata per conservare la configurazione.
esp_err_t config_nvs_init(void);
// Carica il blob persistente nella struttura indicata dal chiamante.
esp_err_t config_load(app_config_t *config);
// Scrive e rende permanente l'intera configurazione tramite nvs_commit().
esp_err_t config_save(const app_config_t *config);

#endif // PARAM_PERSIST_H
