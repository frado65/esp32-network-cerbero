#include "wifi_manager.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "params_http.h"
#include "sntp.h"
#include "status_indicator.h"

#define AP_SSID "ESP32_Network-Cerbero"
#define AP_PASS "netcer1357"
#define FORCE_AP_BUTTON_PIN 2
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "WIFI_MANAGER";
static EventGroupHandle_t wifi_event_group = NULL;

// Callback invocata in background da ESP-IDF quando cambia lo stato del Wi-Fi o dell'IP
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started. Connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected. Retrying connection...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        initialize_sntp(TAG);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool force_ap_button_pressed(void)
{
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << FORCE_AP_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
    vTaskDelay(pdMS_TO_TICKS(50));

    if (gpio_get_level(FORCE_AP_BUTTON_PIN) != 0) {
        ESP_LOGI(TAG, "Pulsante su GPIO %d non premuto all'avvio.", FORCE_AP_BUTTON_PIN);
        return false;
    }

    ESP_LOGI(TAG, "Pulsante premuto all'avvio. Avvio del test di 3 secondi...");
    buzzer_set_state(true);
    bool released = false;
    for (int sample = 0; sample < 30; ++sample) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(FORCE_AP_BUTTON_PIN) != 0) {
            released = true;
            break;
        }
    }
    buzzer_set_state(false);

    if (!released) {
        ESP_LOGW(TAG, "Pulsante su GPIO %d tenuto premuto per 3 secondi. Forza modalita' Access Point!",
                 FORCE_AP_BUTTON_PIN);
        return true;
    }

    ESP_LOGI(TAG, "Pulsante su GPIO %d rilasciato prima dei 3 secondi. Avvio normale in corso.",
             FORCE_AP_BUTTON_PIN);
    return false;
}

static esp_err_t start_access_point(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = sizeof(AP_SSID) - 1U,
            .channel = 1,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(start_webserver());
    ESP_LOGI(TAG, "Access Point avviato. Collegati a '%s' e apri 192.168.4.1", AP_SSID);
    return ESP_OK;
}

static esp_err_t start_station(const wifi_login_t *login)
{
    esp_netif_create_default_wifi_sta();
    wifi_config_t station_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)station_config.sta.ssid, login->ssid,
            sizeof(station_config.sta.ssid));
    strncpy((char *)station_config.sta.password, login->password,
            sizeof(station_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(start_webserver());
    ESP_LOGI(TAG, "WiFi station initialization completed. Tentativo di connessione in corso...");
    return ESP_OK;
}

esp_err_t wifi_manager_start(const app_config_t *config)
{
    wifi_login_t current_login = {0};
    bool has_current_login = login_save_get_current(&config->wifi_logins, &current_login);
    bool force_ap = force_ap_button_pressed();

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    if (force_ap || !has_current_login || current_login.ssid[0] == '\0') {
        if (force_ap) {
            ESP_LOGW(TAG, "Avvio forzato in modalita' Access Point via hardware.");
        } else {
            ESP_LOGW(TAG, "Nessun SSID configurato. Avvio in modalita' Access Point.");
        }
        return start_access_point();
    }

    ESP_LOGI(TAG, "SSID trovato: %s. Avvio in modalita' Station.", current_login.ssid);
    return start_station(&current_login);
}

void wifi_manager_wait_connected(TickType_t timeout_ticks)
{
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, timeout_ticks);
}
