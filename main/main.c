#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "system_utils.h"
#include "sdkconfig.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 Universal Template...");

    // Initialize NVS (Non-Volatile Storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated or had a new version. Erasing and retrying...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS Initialized.");

    // Initialize system_utils component
    system_utils_init();

    // Read configurations from Kconfig
    ESP_LOGI(TAG, "Loaded Kconfig Configurations:");
    ESP_LOGI(TAG, " - WiFi SSID: %s", CONFIG_WIFI_SSID);
    ESP_LOGI(TAG, " - Blink GPIO Pin: %d", CONFIG_BLINK_GPIO_PIN);

    ESP_LOGI(TAG, "Template execution finished successfully.");
}
