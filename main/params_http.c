#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "params_http.h"
#include "param_persist.h"
#include <esp_http_server.h>
#include "esp_log.h"
#include <string.h>

static const char *TAG = "HTTP_SERVER";
static httpd_handle_t server = NULL;

// Semplice pagina HTML con un po' di CSS inline per renderla leggibile
static const char* form_html = 
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>Impostazioni Rete</title>"
    "<style>body{font-family:Arial,sans-serif;margin:40px;background:#f4f4f9;} "
    "input{display:block;margin-bottom:15px;padding:8px;width:100%;max-width:300px;box-sizing:border-box;} "
    "label{font-weight:bold;} "
    "button{padding:10px 15px;background:#007BFF;color:white;border:none;cursor:pointer;}</style>"
    "</head><body>"
    "<h2>Configurazione ESP32</h2>"
    "<form method='POST' action='/submit'>"
    "<label>SSID WiFi:</label><input type='text' name='ssid' required>"
    "<label>Password WiFi:</label><input type='password' name='pass'>"
    "<label>IP Ping (es. 8.8.8.8):</label><input type='text' name='ping_ip' required>"
    "<label>Host Ping (es. google.com):</label><input type='text' name='ping_host' required>"
    "<button type='submit'>Salva e Riavvia</button>"
    "</form></body></html>";

// Handler per la richiesta GET (Mostra la pagina)
static esp_err_t form_get_handler(httpd_req_t *req) {
    httpd_resp_send(req, form_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler per la richiesta POST (Riceve i dati)
static esp_err_t form_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Legge il body della richiesta
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0'; // Termina la stringa

    ESP_LOGI(TAG, "Ricevuto POST: %s", buf);

    app_config_t config;
    memset(&config, 0, sizeof(app_config_t));

    // Estrae i valori dai campi del form
    httpd_query_key_value(buf, "ssid", config.wifi_ssid, sizeof(config.wifi_ssid));
    httpd_query_key_value(buf, "pass", config.wifi_password, sizeof(config.wifi_password));
    httpd_query_key_value(buf, "ping_ip", config.ping_ip, sizeof(config.ping_ip));
    httpd_query_key_value(buf, "ping_host", config.ping_host, sizeof(config.ping_host));

    // Sostituisce i "+" con gli spazi (codifica URL)
    // Utile se l'SSID contiene spazi
    for(int i=0; i<strlen(config.wifi_ssid); i++) {
        if(config.wifi_ssid[i] == '+') config.wifi_ssid[i] = ' ';
    }

    // Salva nella memoria NVS
    config_save(&config);

    // Risponde all'utente
    const char* resp = "<h2>Configurazione Salvata!</h2><p>Riavvio in corso...</p>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Riavvio del sistema richiesto dall'utente via Web...");
    
    // Aspettiamo 1 secondo per dare il tempo allo stack di rete di inviare effettivamente 
    // la pagina HTML di conferma al browser del telefono prima di "staccare la spina"
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    
    // Riavvia il microcontrollore
    esp_restart(); 

    return ESP_OK;
}

esp_err_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    // Inizializza e avvia il server
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = form_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_get);

        httpd_uri_t uri_post = {
            .uri       = "/submit",
            .method    = HTTP_POST,
            .handler   = form_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_post);

        ESP_LOGI(TAG, "Server HTTP avviato");
        return ESP_OK;
    }
    return ESP_FAIL;
}

void stop_webserver(void) {
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
}
