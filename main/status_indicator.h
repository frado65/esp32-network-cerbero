#ifndef STATUS_INDICATOR_H
#define STATUS_INDICATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Crea la coda e il task che controllano LED diagnostico e buzzer. */
esp_err_t status_indicator_init(void);

/* Avvia o sostituisce un'animazione senza bloccare il task chiamante. */
void led_blink_start(uint32_t period_on_ms, uint32_t period_off_ms,
                     uint32_t duration_ms, bool use_buzzer);

/* Controllo diretto usato durante il test del pulsante all'avvio. */
void buzzer_set_state(bool on);

#endif // STATUS_INDICATOR_H
