#include "param_persist.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "PERSIST";
static const char *NVS_NAMESPACE = "storage";
static const char *NVS_KEY = "app_config";

esp_err_t config_nvs_init(void) {
    // Inizializza la partizione NVS (tenta di montare la partizione NVS)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Se la partizione è corrotta o cambiata, formattala e riprova
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_load(app_config_t *config) {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Pulisce la struct con valori di default vuoti in caso di fallimento
    memset(config, 0, sizeof(app_config_t));
    login_save_init(&config->wifi_logins);

    // Apre la partizione NVS in lettura (i namespace sono delle specie di cartelle):
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;

    size_t required_size = sizeof(app_config_t);
    // Lettura del blocco di memoria:
    err = nvs_get_blob(my_handle, NVS_KEY, config, &required_size);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configurazione caricata con successo");
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // probabilmente la scheda è nuova e non ha mai salvato i dati.
        ESP_LOGW(TAG, "Nessuna configurazione trovata. Uso i default.");
    }
    
    nvs_close(my_handle);
    return err;
}

esp_err_t config_save(const app_config_t *config) {
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(my_handle, NVS_KEY, config, sizeof(app_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
        ESP_LOGI(TAG, "Configurazione salvata su NVS");
    }
    
    nvs_close(my_handle);
    return err;
}
