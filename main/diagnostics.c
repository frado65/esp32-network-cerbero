#include "diagnostics.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "network_check.h"
#include "sntp.h"
#include "status_indicator.h"
#include "wifi_manager.h"

#define LOOP_TIME_MS 10000

static const char *TAG = "DIAGNOSTICS";
static const app_config_t *device_config = NULL;
static TickType_t previous_cycle_start = 0;
static DiagnosisEntry previous_diagnosis = {0};

static void wait_for_valid_time(void)
{
    const time_t FIRST_JANUARY_2000 = 946684800;
    const int RETRY_DELAY_MS = 1000;
    const int MAX_RETRIES = 10;
    int retry = 0;
    time_t now;
    time(&now);

    if (now >= FIRST_JANUARY_2000) {
        ESP_LOGI(TAG, "Orario valido già rilevato.");
        return;
    }

    ESP_LOGI(TAG, "Attesa dell'orario sincronizzato (SNTP) ... ");
    while (now < FIRST_JANUARY_2000 && retry < MAX_RETRIES) {
        ++retry;
        vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        time(&now);
        ESP_LOGI(TAG, " .. ");
    }

    if (now >= FIRST_JANUARY_2000) {
        ESP_LOGI(TAG, "Orario valido rilevato. Avvio diagnostica periodica.");
    } else {
        ESP_LOGW(TAG, "Timeout NTP raggiunto. Procedo comunque con l'orologio non sincronizzato.");
    }
}

static void record_if_changed(const DiagnosisEntry *entry)
{
    if (previous_diagnosis.error_mask != entry->error_mask) {
        ESP_LOGI(TAG, "DBG: >>>> ERROR MASK CHANGED!");
        if (diag_append(*entry)) {
            ESP_LOGW(TAG, "Buffer pieno! Sovrascrittura avvenuta, dati persi.");
        }
    }
    previous_diagnosis = *entry;
}

static void diagnostics_task(void *parameters)
{
    ESP_LOGI(TAG, "Network diagnostics task started. Waiting for WiFi connection...");
    wifi_manager_wait_connected(portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected. Starting periodic network checks.");
    wait_for_valid_time();

    while (1) {
        TickType_t cycle_start = xTaskGetTickCount();
        DiagnosisEntry diagnosis = {0};
        diagnosis.error_mask |= DIAG_BIT_OK;
        time(&diagnosis.timestamp);

        ESP_LOGI(TAG, "--------------------------------------------------");
        print_time(TAG, &diagnosis.timestamp);
        ESP_LOGI(TAG, "Starting sequential diagnostics check...");
        led_blink_start(200, 200, 400, false);
        vTaskDelay(pdMS_TO_TICKS(400));

        uint32_t elapsed_ms = (cycle_start - previous_cycle_start) * portTICK_PERIOD_MS;
        ESP_LOGI(TAG, "DBG: elapsed_ms = %" PRIu32, elapsed_ms);
        previous_cycle_start = cycle_start;
        if (elapsed_ms > LOOP_TIME_MS) {
            diagnosis.error_mask |= DIAG_BIT_TIMEOUT;
        }

        bool test_continue = true;
        ip_addr_t router_ip;
        if (!get_router_ip(&router_ip)) {
            ESP_LOGE(TAG, "LAN Check FAILED: Impossibile determinare l'IP del router!");
            led_blink_start(100, 100, 5000, false);
            diagnosis.error_mask |= DIAG_BIT_LAN_IP;
            test_continue = false;
        }

        if (test_continue) {
            ESP_LOGI(TAG, "Check 1/3: LAN - Pinging router at " IPSTR "...",
                     IP2STR(&router_ip.u_addr.ip4));
            if (!perform_ping(&router_ip)) {
                ESP_LOGE(TAG, "LAN Check FAILED!");
                led_blink_start(100, 100, 5000, false);
                diagnosis.error_mask |= DIAG_BIT_LAN_PING;
                test_continue = false;
            } else {
                ESP_LOGI(TAG, "LAN Check PASSED.");
            }
        }

        if (test_continue) {
            const char *target_ip_string = device_config->ping_ip[0] != '\0'
                ? device_config->ping_ip : "8.8.8.8";
            ip_addr_t wan_ip = {.type = IPADDR_TYPE_V4};
            ip4addr_aton(target_ip_string, &wan_ip.u_addr.ip4);
            ESP_LOGI(TAG, "Check 2/3: WAN - Pinging DNS at %s...", target_ip_string);
            if (!perform_ping(&wan_ip)) {
                ESP_LOGE(TAG, "WAN Check FAILED!");
                led_blink_start(200, 200, 5000, true);
                diagnosis.error_mask |= DIAG_BIT_WAN;
                test_continue = false;
            } else {
                ESP_LOGI(TAG, "WAN Check PASSED.");
            }
        }

        if (test_continue) {
            const char *target_host = device_config->ping_host[0] != '\0'
                ? device_config->ping_host : "google.it";
            ESP_LOGI(TAG, "Check 3/3: DNS - Resolving name %s...", target_host);
            if (!perform_dns_check(target_host)) {
                ESP_LOGE(TAG, "DNS Check FAILED!");
                diagnosis.error_mask |= DIAG_BIT_DNS;
                led_blink_start(500, 500, 5000, true);
            } else {
                ESP_LOGI(TAG, "DNS Check PASSED. Network is fully operational.");
            }
        }

        record_if_changed(&diagnosis);

        TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        int32_t delay_ms = LOOP_TIME_MS - (elapsed * portTICK_PERIOD_MS);
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
}

esp_err_t diagnostics_start(const app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    device_config = config;
    BaseType_t result = xTaskCreatePinnedToCore(
        diagnostics_task, "diagnostics_task", 4096, NULL, 4, NULL, 0);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
