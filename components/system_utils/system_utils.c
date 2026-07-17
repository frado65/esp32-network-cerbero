#include "system_utils.h"
#include "esp_log.h"

static const char *TAG = "system_utils";

void system_utils_init(void) {
    /* ESP_LOGI emette un messaggio informativo identificato dal TAG del modulo. */
    ESP_LOGI(TAG, "System utilities initialized successfully.");
}
