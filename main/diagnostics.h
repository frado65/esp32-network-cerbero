#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "esp_err.h"
#include "param_persist.h"

/* Crea il task che esegue periodicamente i controlli LAN, WAN e DNS. */
esp_err_t diagnostics_start(const app_config_t *config);

#endif // DIAGNOSTICS_H
