// #include "display.h"

#pragma once

#include <stdint.h>
#include <string.h>
#include "esp_err.h"

#include "esp_netif.h"

// Configurazione I2C e Pin (papabili: GPIO 0, 1, 3, 4, 5, 6, 7, 10)
#define I2C_MASTER_SCL_IO           6       // Pin SCL
#define I2C_MASTER_SDA_IO           5       // Pin SDA
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          400000  // Frequenza I2C (400 kHz - Fast Mode)
#define SSD1306_I2C_ADDR            0x3C    // Indirizzo I2C tipico dell'SSD1306
#define MAX_NUM_OF_CHAR             21 // Massimo numero di caratteri in una riga (0..20).
#define MAX_NUM_OF_ROWS             7 // massimo numero di righe (0..6)

/**
 * @brief Inizializza il bus I2C e invia la sequenza di boot all'SSD1306
 */
esp_err_t display_init(void);

/**
 * @brief Pulisce l'intero display (spegne tutti i pixel)
 */
void display_clear(void);

/**
 * @brief Scrive una stringa di testo sul display
 * 
 * @param page La riga/pagina (0-7 per un display 128x64)
 * @param col  La colonna di partenza in pixel (0-127)
 * @param text Stringa ASCII null-terminated
 */
void display_draw_text(uint8_t page, uint8_t col, const char *text, const char *text_end);

static inline void display_draw_access_point(const char* ssid, esp_netif_ip_info_t ip_info) {
    display_clear();
    uint8_t _ixr = 1;
    char buffer[64];
    display_draw_text(_ixr++, 0, "ACCESS POINT", NULL);

    _ixr++;
    snprintf(buffer, sizeof(buffer), " - SSID: %s", ssid);
    display_draw_text(_ixr++ ,0, buffer, buffer+MAX_NUM_OF_CHAR);
    if(strlen(buffer) > MAX_NUM_OF_CHAR) {
        display_draw_text(_ixr++, 0, buffer+MAX_NUM_OF_CHAR, NULL);
    }

    _ixr++;
    snprintf(buffer, sizeof(buffer), " - IP: " IPSTR, IP2STR(&ip_info.ip));
    display_draw_text(_ixr, 0, buffer, NULL);
}

