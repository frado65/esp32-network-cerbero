#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "diagnostics.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "param_persist.h"
#include "status_indicator.h"
#include "system_utils.h"
#include "wifi_manager.h"

/* main.c è il composition root dell'applicazione: inizializza i servizi e ne
 * collega le dipendenze, lasciando l'implementazione ai moduli specializzati. */
static const char *TAG = "main";

// Variabile globale che conterrà i parametri di rete
/* È mantenuta globale perché gli handler HTTP devono aggiornare la stessa
 * istanza letta dal task diagnostico. */
app_config_t g_device_config;

/* Prepara l'orologio nello stesso stato di test usato dal progetto originale,
 * così il task diagnostico può verificare l'avvenuta sincronizzazione SNTP. */
static void initialize_test_clock(void)
{
    ESP_LOGI("DEBUG", "Forzatura dell'orologio di sistema a zero per test...");
    struct timeval tv = {
        .tv_sec = 0,  // Imposta i secondi al 1 Gennaio 1970, ore 00:00:00
        .tv_usec = 0
    };
    settimeofday(&tv, NULL);

    // Configura anche la Timezone a UTC per i test, così localtime() non aggiungerà offset
    // e sposterà l'orario a ore 01:00:00 a causa del fuso orario italiano
    setenv("TZ", "UTC0", 1);
    tzset();

    // Imposta l'aggiornamento del tempo ogni ora (TODO: scablare):
    //esp_sntp_set_sync_interval(60*60*1000);
    esp_sntp_set_sync_interval(1 * 60 * 1000);
}

// Entry point standard di ESP-IDF (su FreeRTOS corrisponde ad un task generato internamente all'avvio)
void app_main(void)
{
    ESP_LOGI(TAG, "Starting sequential diagnostics on ESP32-C3...");
    initialize_test_clock();

    // 1. Inizializza il file system NVS
    ESP_ERROR_CHECK(config_nvs_init());

    // 2. Carica la configurazione dalla memoria Flash
    /* ESP_ERR_NVS_NOT_FOUND è normale al primo avvio: config_load lascia in
     * quel caso la struttura inizializzata con i valori di default. */
    config_load(&g_device_config);

    // Richiama il setup custom presente nel tuo template (system_utils.c)
    system_utils_init();

    // Avvia il task dedicato al LED e al buzzer prima del test del pulsante.
    ESP_ERROR_CHECK(status_indicator_init());

    // Configura AP/Station, registra gli eventi e avvia il server HTTP.
    ESP_ERROR_CHECK(wifi_manager_start(&g_device_config));

    // Avvia infine il task periodico che attenderà la connessione Station.
    ESP_ERROR_CHECK(diagnostics_start(&g_device_config));
}
