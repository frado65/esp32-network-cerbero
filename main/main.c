#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "system_utils.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#define DIAG_LED_PIN CONFIG_BLINK_GPIO_PIN

// LED command structure
typedef struct {
    uint32_t period_on_ms;
    uint32_t period_off_ms;
    uint32_t duration_ms;
} led_blink_cmd_t;

static QueueHandle_t led_cmd_queue = NULL;

// Function to trigger a specific LED pattern
void led_blink_start(uint32_t period_on_ms, uint32_t period_off_ms, uint32_t duration_ms) {
    led_blink_cmd_t cmd = {
        .period_on_ms = period_on_ms,
        .period_off_ms = period_off_ms,
        .duration_ms = duration_ms
    };
    if (led_cmd_queue != NULL) {
        xQueueOverwrite(led_cmd_queue, &cmd);
    }
}

// LED Blink Task: manages active-low blinking durations non-blockingly
static void led_blink_task(void *pvParameters) {
    led_blink_cmd_t current_cmd = {0};
    bool is_blinking = false;
    TickType_t start_tick = 0;

    // Configure GPIO pin
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DIAG_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Initial state: OFF (Active-Low = HIGH)
    gpio_set_level(DIAG_LED_PIN, 1);

    while (1) {
        led_blink_cmd_t new_cmd;
        // Wait on the queue. If blinking, don't block so we can continue the blink cycle.
        TickType_t wait_ticks = is_blinking ? 0 : portMAX_DELAY;

        if (xQueueReceive(led_cmd_queue, &new_cmd, wait_ticks) == pdPASS) {
            current_cmd = new_cmd;
            is_blinking = (current_cmd.duration_ms > 0);
            start_tick = xTaskGetTickCount();
            if (is_blinking) {
                gpio_set_level(DIAG_LED_PIN, 0); // Start with LED ON (LOW)
            } else {
                gpio_set_level(DIAG_LED_PIN, 1); // LED OFF (HIGH)
            }
        }

        if (is_blinking) {
            uint32_t elapsed_ms = (xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;
            if (elapsed_ms >= current_cmd.duration_ms) {
                // Duration expired, reset to resting state (OFF = HIGH)
                gpio_set_level(DIAG_LED_PIN, 1);
                is_blinking = false;
            } else {
                uint32_t period = current_cmd.period_on_ms + current_cmd.period_off_ms;
                if (period > 0) {
                    uint32_t phase = elapsed_ms % period;
                    if (phase < current_cmd.period_on_ms) {
                        gpio_set_level(DIAG_LED_PIN, 0); // LED ON (LOW)
                    } else {
                        gpio_set_level(DIAG_LED_PIN, 1); // LED OFF (HIGH)
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10)); // Check phase every 10ms
            }
        }
    }
}

// Wi-Fi Configuration and Event Group
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started. Connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected. Retrying connection...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi station initialization completed.");
}

// Ping Context and Callbacks
typedef struct {
    SemaphoreHandle_t sem;
    bool success;
} ping_ctx_t;

static void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args) {
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    ctx->success = true;
}

static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    // Keep ctx->success false
}

static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args) {
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    xSemaphoreGive(ctx->sem);
}

// Helper function to perform a single ping
bool perform_ping(const ip_addr_t *target_ip) {
    ping_ctx_t ctx = {
        .sem = xSemaphoreCreateBinary(),
        .success = false
    };
    if (!ctx.sem) {
        return false;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = *target_ip;
    ping_config.count = 1;
    ping_config.timeout_ms = 1000; // 1 second timeout

    esp_ping_callbacks_t cbs = {
        .on_ping_success = cmd_ping_on_ping_success,
        .on_ping_timeout = cmd_ping_on_ping_timeout,
        .on_ping_end = cmd_ping_on_ping_end,
        .cb_args = &ctx
    };

    esp_ping_handle_t ping_handle;
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(ctx.sem);
        return false;
    }

    esp_ping_start(ping_handle);

    // Wait for the ping session to complete
    if (xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(1200)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping semaphore take timeout");
    }

    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    vSemaphoreDelete(ctx.sem);

    return ctx.success;
}

// Diagnostics Task: runs sequentially every 10 seconds
static void diagnostics_task(void *pvParameters) {
    ESP_LOGI(TAG, "Network diagnostics task started. Waiting for WiFi connection...");
    
    // Wait until the device gets connected to the WiFi AP initially
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected. Starting periodic network checks.");

    while (1) {
        TickType_t cycle_start = xTaskGetTickCount();

        // 1. Confirm test: Single flash (200ms ON / 200ms OFF)
        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "Starting sequential diagnostics check...");
        led_blink_start(200, 200, 400);
        vTaskDelay(pdMS_TO_TICKS(400)); // wait for single flash to finish

        // 2. Check LAN (Router): Ping 192.168.202.111
        ip_addr_t router_ip;
        ip4addr_aton("192.168.202.111", &router_ip.u_addr.ip4);
        router_ip.type = IPADDR_TYPE_V4;

        ESP_LOGI(TAG, "Check 1/3: LAN - Pinging router at 192.168.202.111...");
        if (!perform_ping(&router_ip)) {
            ESP_LOGE(TAG, "LAN Check FAILED!");
            led_blink_start(100, 100, 5000); // Fast blink (100ms ON / 100ms OFF) for 5 seconds
            
            // Calculate delay for next cycle
            TickType_t elapsed = xTaskGetTickCount() - cycle_start;
            int32_t delay_ms = 10000 - (elapsed * portTICK_PERIOD_MS);
            if (delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
            continue;
        }
        ESP_LOGI(TAG, "LAN Check PASSED.");

        // 3. Check WAN (Routing): Ping 8.8.8.8
        ip_addr_t wan_ip;
        ip4addr_aton("8.8.8.8", &wan_ip.u_addr.ip4);
        wan_ip.type = IPADDR_TYPE_V4;

        ESP_LOGI(TAG, "Check 2/3: WAN - Pinging DNS at 8.8.8.8...");
        if (!perform_ping(&wan_ip)) {
            ESP_LOGE(TAG, "WAN Check FAILED!");
            led_blink_start(200, 200, 5000); // Medium blink (200ms ON / 200ms OFF) for 5 seconds
            
            TickType_t elapsed = xTaskGetTickCount() - cycle_start;
            int32_t delay_ms = 10000 - (elapsed * portTICK_PERIOD_MS);
            if (delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
            continue;
        }
        ESP_LOGI(TAG, "WAN Check PASSED.");

        // 4. Check DNS: resolve and check reachability of google.it
        ESP_LOGI(TAG, "Check 3/3: DNS - Resolving name google.it...");
        struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
        };
        struct addrinfo *res = NULL;
        int err = getaddrinfo("google.it", NULL, &hints, &res);
        bool dns_success = false;

        if (err == 0 && res != NULL) {
            struct in_addr *addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
            char ip_str[16];
            inet_ntoa_r(*addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Resolved google.it to %s. Verifying reachability...", ip_str);

            ip_addr_t dns_target_ip;
            dns_target_ip.type = IPADDR_TYPE_V4;
            dns_target_ip.u_addr.ip4.addr = addr->s_addr;

            if (perform_ping(&dns_target_ip)) {
                dns_success = true;
            } else {
                ESP_LOGE(TAG, "Ping to resolved google.it IP address failed.");
            }
            freeaddrinfo(res);
        } else {
            ESP_LOGE(TAG, "DNS resolution of google.it failed with code: %d", err);
        }

        if (!dns_success) {
            ESP_LOGE(TAG, "DNS Check FAILED!");
            led_blink_start(500, 500, 5000); // Slow blink (500ms ON / 500ms OFF) for 5 seconds
        } else {
            ESP_LOGI(TAG, "DNS Check PASSED. Network is fully operational.");
        }

        // Wait until the end of the 10-second cycle
        TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        int32_t delay_ms = 10000 - (elapsed * portTICK_PERIOD_MS);
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting sequential diagnostics on ESP32-C3...");

    // Initialize NVS (Non-Volatile Storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize system_utils component
    system_utils_init();

    // Create the LED control command queue
    led_cmd_queue = xQueueCreate(1, sizeof(led_blink_cmd_t));
    if (led_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED command queue.");
        return;
    }

    // Spawn non-blocking LED management task
    xTaskCreatePinnedToCore(led_blink_task, "led_blink_task", 3072, NULL, 5, NULL, 0);

    // Initialize Wi-Fi
    wifi_init_sta();

    // Spawn diagnostics task
    xTaskCreatePinnedToCore(diagnostics_task, "diagnostics_task", 4096, NULL, 4, NULL, 0);
}
