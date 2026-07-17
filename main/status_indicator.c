#include "status_indicator.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BUZZER_PIN 3
#define DIAG_LED_PIN 8

static const char *TAG = "STATUS_INDICATOR";

// Struttura dati che definisce il "comando" di lampeggio
typedef struct {
    uint32_t period_on_ms;  // Tempo di accensione in millisecondi
    uint32_t period_off_ms; // Tempo di spegnimento in millisecondi
    uint32_t duration_ms;   // Durata totale dell'animazione
    bool enable_buzzer;     // Abilita/disabilita il buzzer
} led_blink_cmd_t;

// Handle per la coda di messaggi usata per comunicare con il task del LED
static QueueHandle_t g_led_cmd_queue = NULL;

static void buzzer_init(void)
{
    // 1. Configura il timer PWM
    ledc_timer_config_t _ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 4150,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&_ledc_timer));

    // 2. Collega il timer al pin del buzzer
    ledc_channel_config_t _ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BUZZER_PIN,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&_ledc_channel));
}

void buzzer_set_state(bool on)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 4096 : 0);
    /* set_duty prepara il nuovo valore; update_duty lo applica all'hardware. */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// Funzione helper per inviare un nuovo comando di lampeggio al task del LED
void led_blink_start(uint32_t period_on_ms, uint32_t period_off_ms,
                     uint32_t duration_ms, bool use_buzzer)
{
    led_blink_cmd_t _cmd = {
        .period_on_ms = period_on_ms,
        .period_off_ms = period_off_ms,
        .duration_ms = duration_ms,
        .enable_buzzer = use_buzzer
    };
    // xQueueOverwrite sovrascrive l'ultimo messaggio se la coda è piena (in questo caso è di 1 solo elemento).
    // Questo permette di interrompere un lampeggio in corso con uno nuovo senza blocchi.
    if (g_led_cmd_queue != NULL) {
        xQueueOverwrite(g_led_cmd_queue, &_cmd);
    }
}

// Task FreeRTOS indipendente che gestisce il lampeggio del LED senza bloccare il resto del codice
static void led_blink_task(void *pvParameters)
{
    led_blink_cmd_t _current_cmd = {0};
    bool _is_blinking = false;
    TickType_t _start_tick = 0;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DIAG_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    buzzer_init();
    gpio_set_level(DIAG_LED_PIN, 1);
    buzzer_set_state(false);

    while (1) {
        led_blink_cmd_t _new_cmd;
        TickType_t _wait_ticks = _is_blinking ? 0 : portMAX_DELAY;
        if (xQueueReceive(g_led_cmd_queue, &_new_cmd, _wait_ticks) == pdPASS) {
            _current_cmd = _new_cmd;
            _is_blinking = _current_cmd.duration_ms > 0;
            _start_tick = xTaskGetTickCount();
            gpio_set_level(DIAG_LED_PIN, _is_blinking ? 0 : 1);
            buzzer_set_state(_is_blinking && _current_cmd.enable_buzzer);
        }

        if (_is_blinking) {
            uint32_t _elapsed_ms = (xTaskGetTickCount() - _start_tick) * portTICK_PERIOD_MS;
            if (_elapsed_ms >= _current_cmd.duration_ms) {
                gpio_set_level(DIAG_LED_PIN, 1);
                buzzer_set_state(false);
                _is_blinking = false;
            } else {
                uint32_t period = _current_cmd.period_on_ms + _current_cmd.period_off_ms;
                if (period > 0) {
                    uint32_t phase = _elapsed_ms % period;
                    bool on = phase < _current_cmd.period_on_ms;
                    gpio_set_level(DIAG_LED_PIN, on ? 0 : 1);
                    buzzer_set_state(on && _current_cmd.enable_buzzer);
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

esp_err_t status_indicator_init(void)
{
    g_led_cmd_queue = xQueueCreate(1, sizeof(led_blink_cmd_t));
    if (g_led_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED command queue.");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        led_blink_task, "led_blink_task", 3072, NULL, 5, NULL, 0);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
