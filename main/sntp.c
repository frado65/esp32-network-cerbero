#include "sntp.h"

#include <stdlib.h>

#include "esp_log.h"
#include "esp_sntp.h"

void initialize_sntp(const char *tag)
{
    ESP_LOGI(tag, "Inizializzazione SNTP...");

    // Inizializza il client sntp
    /* In modalità POLL il dispositivo interroga periodicamente il server,
     * senza comportarsi a sua volta come server dell'orario. */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    // Imposta il server (puoi usarne più di uno)
    esp_sntp_setservername(0, "pool.ntp.org");

    // Inizializza il servizio
    esp_sntp_init();

    // Imposta il fuso orario (esempio per l'Italia) - TODO: scablare ma mettere come default.
    /* setenv definisce le regole CET/CEST; tzset le carica nella libreria C,
     * così localtime/localtime_r applicano fuso e ora legale italiani. */
    setenv("TZ", "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);
    tzset();

    ESP_LOGI(tag, "SNTP inizializzato. Sincronizzazione in corso...");
}

void print_time(const char *tag, time_t *p_now)
{
    //time_t now; // alla fine è un long int.
    struct tm _timeinfo;
    /* time aggiorna l'epoch; localtime_r produce una struct tm usando una
     * variante rientrante, sicura rispetto all'uso concorrente tra task. */
    //time(p_now);
    localtime_r(p_now, &_timeinfo);

    // _timeinfo.tm_hour, _timeinfo.tm_min, _timeinfo.tm_sec contengono l'ora corretta
    char _strftime_buf[64];
    /* strftime converte i campi numerici di struct tm in testo leggibile. */
    strftime(_strftime_buf, sizeof(_strftime_buf), "%c", &_timeinfo);
    ESP_LOGI(tag, "Ora attuale: %s", _strftime_buf);
}
