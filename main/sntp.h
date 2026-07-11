#pragma once

#include "esp_sntp.h"
#include <time.h>
#include "esp_log.h"

void initialize_sntp(const char* tag) {
    ESP_LOGI(tag, "Inizializzazione SNTP...");
    
    // Inizializza il client sntp
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    
    // Imposta il server (puoi usarne più di uno)
    esp_sntp_setservername(0, "pool.ntp.org");
    
    // Inizializza il servizio
    esp_sntp_init();
    
    // Imposta il fuso orario (esempio per l'Italia) - TODO: scablare ma mettere come default.
    setenv("TZ", "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);
    tzset();
    
    ESP_LOGI(tag, "SNTP inizializzato. Sincronizzazione in corso...");
}

void print_time(const char* tag, time_t* p_now) {
    //time_t now; // alla fine è un long int.
    struct tm timeinfo;
    time(p_now);
    localtime_r(p_now, &timeinfo);

    // timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec contengono l'ora corretta
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(tag, "Ora attuale: %s", strftime_buf);
}
