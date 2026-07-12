#include <esp_http_server.h>
#include <string.h>
#include <time.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "params_http.h"
#include "param_persist.h"
#include "esp_log.h"

#include "event_log.h"


static const char *TAG = "HTTP_SERVER";
static httpd_handle_t server = NULL;

extern app_config_t g_device_config;

// Una favicon SVG leggera e definita (Smile moderno)
static const char favicon_svg[] = 
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
    "<circle cx='12' cy='12' r='10' fill='#FFD43B'/>"
    "<circle cx='8.5' cy='9.5' r='1.5' fill='#2B2D42'/>"
    "<circle cx='15.5' cy='9.5' r='1.5' fill='#2B2D42'/>"
    "<path d='M8 14.5s2.5 3 4 3 4-3 4-3' fill='none' stroke='#2B2D42' stroke-width='2' stroke-linecap='round'/>"
    "</svg>";

// Template HTML: i %s verranno sostituiti a runtime con i valori attuali
static const char* form_template = 
    "... (header e CSS) ..."
    "<h2>Configurazione Operativa (Hot)</h2>"
    "<form method='POST' action='/submit_hot'>"
    "<label>IP Ping:</label><input type='text' name='ping_ip' value='%s'>"
    "<label>Host Ping:</label><input type='text' name='ping_host' value='%s'>"
    "<button type='submit'>Applica Ora</button></form>"
    "<hr>"
    "<h2>Configurazione Rete (Cold - Riavvio)</h2>"
    "<form method='POST' action='/submit_cold'>"
    "<label>SSID WiFi:</label><input type='text' name='ssid' value='%s'>"
    "<label>Password:</label><input type='password' name='pass' value='%s'>"
    "<button type='submit'>Salva e Riavvia</button></form></body></html>";


// Converte un byte in una stringa di 8 caratteri binari + \0
static void byte_to_binary_str(uint8_t byte, char *out_str) {
    for (int i = 7; i >= 0; i--) {
        out_str[7 - i] = (byte & (1 << i)) ? '1' : '0';
    }
    out_str[8] = '\0';
}


// Handler per la richiesta GET (Mostra la pagina con i dati compilati)
static esp_err_t form_get_handler(httpd_req_t *req) {
    // Alloca 1024 byte per contenere la pagina completa
    char *resp_str = malloc(1024);
    if (resp_str == NULL) {
        ESP_LOGE(TAG, "Impossibile allocare memoria per la pagina HTML");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Inietta i valori attuali nel template HTML
    snprintf(resp_str, 1024, form_template, 
             g_device_config.ping_ip, 
             g_device_config.ping_host,
             g_device_config.wifi_ssid, 
             g_device_config.wifi_password
            );

    // Invia la pagina popolata
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    
    // Libera la memoria dinamica
    free(resp_str);
    return ESP_OK;
}

// Handler per parametri "Hot" (Applicazione immediata)
static esp_err_t form_hot_handler(httpd_req_t *req) {
    char buf[256] = {0};

    const int _ret = httpd_req_recv(req, buf, sizeof(buf)); // (omettendo error checking per brevità)
    if (_ret <= 0) {
        return ESP_FAIL;
    }
    buf[_ret] = '\0'; // Garantisce che il parsing non legga oltre i dati ricevuti
    
    // AZZERA PRIMA DI SCRIVERE: garantisce che i vecchi caratteri non restino lì
    memset(g_device_config.ping_ip, 0, sizeof(g_device_config.ping_ip));
    memset(g_device_config.ping_host, 0, sizeof(g_device_config.ping_host));

    // Aggiorna la struttura globale in RAM
    httpd_query_key_value(buf, "ping_ip", g_device_config.ping_ip, sizeof(g_device_config.ping_ip));
    httpd_query_key_value(buf, "ping_host", g_device_config.ping_host, sizeof(g_device_config.ping_host));

    ESP_LOGI(TAG, "DEBUG3: Valore estratto: [%s] | Hex: %02x %02x %02x", 
         g_device_config.ping_host, 
         g_device_config.ping_host[strlen(g_device_config.ping_host)+1],
         g_device_config.ping_host[strlen(g_device_config.ping_host)+2],
         g_device_config.ping_host[strlen(g_device_config.ping_host)+3]);   
          
    // Salva in NVS senza riavviare
    config_save(&g_device_config); 

    httpd_resp_send(req, "<h2>Configurazione aggiornata!</h2><a href='/'>Torna indietro</a>", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler per parametri "Cold" (Richiede riavvio)
static esp_err_t form_cold_handler(httpd_req_t *req) {
    char buf[256] = {0};
    const int _remaining = req->content_len;
    int _ret = _remaining;


    if (_remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Legge il body della richiesta
    _ret = httpd_req_recv(req, buf, _remaining);
    if (_ret <= 0) {
        return ESP_FAIL;
    }
    buf[_ret] = '\0'; // Termina la stringa

    ESP_LOGI(TAG, "Ricevuto POST: %s", buf);

    app_config_t config;
    memcpy(&config, &g_device_config, sizeof(app_config_t));
    memset(config.wifi_ssid, 0, sizeof(config.wifi_ssid));
    memset(config.wifi_password, 0, sizeof(config.wifi_password));
    
    // Estrae i valori dai campi del form
    httpd_query_key_value(buf, "ssid", config.wifi_ssid, sizeof(config.wifi_ssid));
    httpd_query_key_value(buf, "pass", config.wifi_password, sizeof(config.wifi_password));
    //httpd_query_key_value(buf, "ping_ip", config.ping_ip, sizeof(config.ping_ip));
    //httpd_query_key_value(buf, "ping_host", config.ping_host, sizeof(config.ping_host));

    // Sostituisce i "+" con gli spazi (codifica URL)
    // Utile se l'SSID contiene spazi
    for(int i=0; i<strlen(config.wifi_ssid); i++) {
        if(config.wifi_ssid[i] == '+') config.wifi_ssid[i] = ' ';
    }

    // Salva nella memoria NVS
    config_save(&config);    
    httpd_resp_send(req, "<h2>Salvato. Riavvio in corso...</h2>", HTTPD_RESP_USE_STRLEN);
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}


static esp_err_t favicon_get_handler(httpd_req_t *req) {
    // Specifichiamo al browser che è un'immagine vettoriale SVG
    httpd_resp_set_type(req, "image/svg+xml");
    
    // Inviamo la stringa usando strlen visto che è testo puro
    httpd_resp_send(req, favicon_svg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


// Handler per mostrare la tabella dei dati diagnostici
static esp_err_t diag_page_handler(httpd_req_t *req) {
    // Allocazione dinamica per contenere la pagina HTML con la tabella
    // 2048 o più a seconda di quanti tag HTML vuoi inserire
    char *resp_str = malloc(3072);
    if (resp_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

// Inizio pagina HTML con lo script di refresh automatico sul click
    int len = snprintf(resp_str, 3072,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Diagnostica</title>"
        "<style>body{font-family:sans-serif;margin:20px;} table{width:100%%;border-collapse:collapse;} "
        "th,td{border:1px solid #ccc;padding:8px;text-align:left;} th{background:#f2f2f2;}</style></head>"
        "<body><h2>Registro Diagnostica</h2>"
        
        // IL TRUCCO: Al click, avvia il download e dopo 500ms ricarica la pagina corrente
        "<button style='padding:10px;margin-bottom:15px;cursor:pointer;' "
        "onclick=\"location.href='/diag/download'; setTimeout(function(){ location.reload(); }, 500);\">"
        "Scarica CSV e Svuota</button>"
        
        " | <a href='/'>Torna alla Home</a><br><br>"
        "<table><tr><th>#</th><th>Timestamp</th><th>Error Mask (Bin)</th></tr>");
        
    uint16_t count = diag_get_count();
    if (count == 0) {
        len += snprintf(resp_str + len, 3072 - len, "<tr><td colspan='3'>Nessun dato registrato.</td></tr>");
    } else {
        for (uint16_t i = 0; i < count; i++) {
            DiagnosisEntry entry;
            if (diag_extract(i, &entry)) {
                if (3072 - len < 150) break; // Margine di sicurezza aumentato per le stringhe più lunghe
                
                // 1. Conversione del Timestamp
                char time_buf[24];
                time_t t = (time_t)entry.timestamp; // Se è in millisecondi, fai: entry.timestamp / 1000;
                struct tm *tm_info = localtime(&t);
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%m-%S", tm_info);

                // 2. Conversione dell'Error Mask (prendiamo solo gli 8 bit meno significativi)
                char bin_buf[9];
                byte_to_binary_str((uint8_t)(entry.error_mask & 0xFF), bin_buf);
                
                len += snprintf(resp_str + len, 3072 - len,
                    "<tr><td>%d</td><td>%s</td><td><code>%s</code></td></tr>",
                    i + 1, time_buf, bin_buf);
            }
        }
    }

    // Chiusura tag HTML
    snprintf(resp_str + len, 3072 - len, "</table></body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    
    free(resp_str);
    return ESP_OK;
}

// Handler per il download del CSV e successivo wipe del buffer
static esp_err_t diag_download_csv_handler(httpd_req_t *req) {
    // Allochiamo un buffer temporaneo per chunk di testo o per l'intero file.
    // Con 64 entry massime, il CSV intero occuperà circa 1.5 - 2 KB.
    char *csv_buf = malloc(2048);
    if (csv_buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Forza il browser a scaricare il contenuto come file invece di visualizzarlo
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=diagnostica_cerbero.csv");

    int len = snprintf(csv_buf, 2048, "Indice;Timestamp;ErrorMask\n");

    uint16_t count = diag_get_count();
    for (uint16_t i = 0; i < count; i++) {
        DiagnosisEntry entry;
        if (diag_extract(i, &entry)) {
            if (2048 - len < 80) break;

            // 1. Conversione del Timestamp
            char time_buf[24];
            time_t t = (time_t)entry.timestamp; // Se è in millisecondi: entry.timestamp / 1000;
            struct tm *tm_info = localtime(&t);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%m-%S", tm_info);

            // 2. Conversione dell'Error Mask
            char bin_buf[9];
            byte_to_binary_str((uint8_t)(entry.error_mask & 0xFF), bin_buf);

            len += snprintf(csv_buf + len, 2048 - len, "%d;%s;%s\n",
                            i + 1, time_buf, bin_buf);
        }
    }

    // Spedisci il file CSV creato
    httpd_resp_send(req, csv_buf, len);
    free(csv_buf);

    // !! AZIONE CRITICA !! 
    // Svuota l'array solo dopo che l'invio della risposta HTTP è andato a buon fine
    diag_clear();
    ESP_LOGI(TAG, "Buffer diagnostico scaricato e svuotato.");

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

        httpd_uri_t uri_hot = {
            .uri       = "/submit_hot",
            .method    = HTTP_POST,
            .handler   = form_hot_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_hot);

        httpd_uri_t uri_cold = {
            .uri       = "/submit_cold",
            .method    = HTTP_POST,
            .handler   = form_cold_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_cold);

        httpd_uri_t uri_favicon = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_favicon);

        // URI per la visualizzazione della Tabella Diagnostica
        httpd_uri_t uri_diag = {
            .uri       = "/diag",
            .method    = HTTP_GET,
            .handler   = diag_page_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_diag);

        // URI per il download del CSV
        httpd_uri_t uri_diag_down = {
            .uri       = "/diag/download",
            .method    = HTTP_GET,
            .handler   = diag_download_csv_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_diag_down);

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
