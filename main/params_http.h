#ifndef PARAMS_HTTP_H
#define PARAMS_HTTP_H

#include "esp_err.h"

/* Avvia il task HTTP, registra tutte le rotte e restituisce ESP_OK se il
 * server è pronto ad accettare richieste. */
esp_err_t start_webserver(void);
/* Arresta il server e libera le risorse associate al relativo handle. */
void stop_webserver(void);

#endif // PARAMS_HTTP_H
