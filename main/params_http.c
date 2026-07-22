#include <esp_http_server.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "params_http.h"
#include "param_persist.h"
#include "esp_log.h"

#include "event_log.h"

/* Il modulo espone pagine di configurazione e diagnostica. Gli handler sono
 * eseguiti nel task interno di esp_http_server e restituiscono esp_err_t. */
#define BUFFER_SIZE 5120
#define LOGIN_OPTIONS_BUFFER_SIZE 2304

static const char *TAG = "HTTP_SERVER";
/* Handle opaco del g_server: NULL indica che non esiste un'istanza attiva. */
static httpd_handle_t g_server = NULL;

extern app_config_t g_device_config;

// Una favicon SVG leggera e definita (Smile moderno)
static const char g_favicon_svg[] = 
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
    "<circle cx='12' cy='12' r='10' fill='#FFD43B'/>"
    "<circle cx='8.5' cy='9.5' r='1.5' fill='#2B2D42'/>"
    "<circle cx='15.5' cy='9.5' r='1.5' fill='#2B2D42'/>"
    "<path d='M8 14.5s2.5 3 4 3 4-3 4-3' fill='none' stroke='#2B2D42' stroke-width='2' stroke-linecap='round'/>"
    "</svg>";

// Template HTML: i %s verranno sostituiti a runtime con i valori attuali
static const char* g_form_template = 
    /* I tre segnaposto %s vengono sostituiti con IP, host e lista degli SSID;
     * eventuali '%' letterali nel template devono essere scritti come '%%'. */
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Configurazione</title>"
    "<style>"
    "body{font-family:sans-serif;margin:20px;}"
    "h2{color:#333;}"
    "label{display:block;margin-top:10px;}"
    "input,select{margin-bottom:10px;padding:5px;width:265px;}"
    "button{display:block;padding:8px 15px;margin-top:10px;cursor:pointer;}"
    "</style></head><body>"
    "<a href='/'>HOME</a>"
    "<h2>Configurazione Operativa (Hot)</h2>"
    "<form method='POST' action='/submit_hot'>"
    "<label>IP Ping:</label><input type='text' name='ping_ip' value='%s'>"
    "<label>Host Ping:</label><input type='text' name='ping_host' value='%s'>"
    "<button type='submit'>Applica Ora</button></form>"
    "<hr>"
    "<h2>Configurazione Rete (Cold - Riavvio)</h2>"
    "<form method='POST' action='/submit_cold'>"
    "<label>Hotspot salvato:</label><select name='login_ixp' id='login_ixp' "
    "onchange='toggleNewLogin()'>%s</select>"
    "<div id='new_login'>"
    "<label>Nuovo SSID WiFi:</label><input type='text' name='ssid' maxlength='31'>"
    "<label>Nuova password:</label><input type='password' name='pass' maxlength='63'>"
    "</div>"
    "<script>function toggleNewLogin(){var n=document.getElementById('login_ixp').value==='-1';"
    "document.getElementById('new_login').style.display=n?'block':'none';}toggleNewLogin();</script>"
    "<button type='submit'>Salva e Riavvia</button></form></body></html>";

static size_t html_escape(const char *source, char *destination, size_t destination_size)
{
    /* Sostituisce i caratteri con significato HTML per impedire che un SSID
     * venga interpretato come markup nella pagina di configurazione. */
    size_t _written = 0;

    if (destination_size == 0U) {
        return 0U;
    }

    while (*source != '\0') {
        const char *_replacement = NULL;
        switch (*source) {
            case '&': _replacement = "&amp;"; break;
            case '<': _replacement = "&lt;"; break;
            case '>': _replacement = "&gt;"; break;
            case '\"': _replacement = "&quot;"; break;
            case '\'': _replacement = "&#39;"; break;
            default: break;
        }

        const size_t _chunk_size = _replacement != NULL ? strlen(_replacement) : 1U;
        if (_written + _chunk_size >= destination_size) {
            break;
        }

        if (_replacement != NULL) {
            memcpy(destination + _written, _replacement, _chunk_size);
        } else {
            destination[_written] = *source;
        }
        _written += _chunk_size;
        ++source;
    }

    destination[_written] = '\0';
    return _written;
}

static bool receive_request_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    /* content_len proviene dall'header HTTP. Si riserva sempre un byte per il
     * terminatore NUL, necessario al successivo parsing come query string. */
    if ((size_t)req->content_len >= buffer_size) {
        return false;
    }

    size_t _received = 0;
    while (_received < (size_t)req->content_len) {
        const int _result = httpd_req_recv(req, buffer + _received,
                                          (size_t)req->content_len - _received);
        if (_result == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Un timeout temporaneo non chiude necessariamente la connessione:
             * si riprova finché arriva tutto il body dichiarato. */
            continue;
        }
        if (_result <= 0) {
            return false;
        }
        _received += (size_t)_result;
    }

    buffer[_received] = '\0';
    return true;
}

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodifica in-place una stringa URL-encoded (converte '+' in spazio e '%XX' nel corrispondente carattere ASCII) */
static void url_decode(char *str)
{
    if (str == NULL) {
        return;
    }

    char *src = str;
    char *dst = str;

    while (*src != '\0') {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
            const int h1 = hex_to_int(src[1]);
            const int h2 = hex_to_int(src[2]);
            if (h1 >= 0 && h2 >= 0) {
                *dst++ = (char)((h1 << 4) | h2);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}


// Converte un byte in una stringa di 8 caratteri binari + \0
static void byte_to_binary_str(uint8_t byte, char *out_str) {
    /* Visita i bit dal più significativo al meno significativo e genera
     * esattamente otto caratteri, seguiti dal terminatore di stringa. */
    for (int i = 7; i >= 0; i--) {
        out_str[7 - i] = (byte & (1 << i)) ? '1' : '0';
    }
    out_str[8] = '\0';
}


// Handler per la richiesta GET (Mostra la pagina con i dati compilati)
static esp_err_t form_get_handler(httpd_req_t *req) {
    // Alloca BUFFER_SIZE byte per contenere la pagina completa
    char *_resp_str = malloc(BUFFER_SIZE);
    if (_resp_str == NULL) {
        ESP_LOGE(TAG, "Impossibile allocare memoria per la pagina HTML");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* Il buffer è sull'heap per non consumare lo stack limitato del task HTTP;
     * calloc lo inizializza anche come stringa vuota valida. */
    char *login_options = calloc(1, LOGIN_OPTIONS_BUFFER_SIZE);
    if (login_options == NULL) {
        ESP_LOGE(TAG, "Impossibile allocare memoria per le opzioni WiFi");
        free(_resp_str);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const int initial_result = snprintf(
        login_options, LOGIN_OPTIONS_BUFFER_SIZE,
        "<option value='-1'%s>-- Inserisci un nuovo hotspot --</option>",
        g_device_config.wifi_logins.current_ixp == LOGIN_SAVE_NOT_FOUND ? " selected" : "");
    if (initial_result < 0 || initial_result >= LOGIN_OPTIONS_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Buffer delle opzioni WiFi insufficiente");
        free(login_options);
        free(_resp_str);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t options_length = (size_t)initial_result;

    const size_t login_count = login_save_length(&g_device_config.wifi_logins);
    /* Ogni credenziale salvata diventa una voce della select HTML. */
    for (size_t ixp = 0; ixp < login_count; ++ixp) {
        wifi_login_t login = {0};
        char escaped_ssid[(WIFI_SSID_BUFFER_SIZE * 6U) + 1U];
        if (!login_save_get(&g_device_config.wifi_logins, ixp, &login)) {
            continue;
        }
        html_escape(login.ssid, escaped_ssid, sizeof(escaped_ssid));

        const int _result = snprintf(
            login_options + options_length, LOGIN_OPTIONS_BUFFER_SIZE - options_length,
            "<option value='%u'%s>%s</option>", (unsigned)ixp,
            g_device_config.wifi_logins.current_ixp == (int32_t)ixp ? " selected" : "",
            escaped_ssid);
        if (_result < 0 || (size_t)_result >= LOGIN_OPTIONS_BUFFER_SIZE - options_length) {
            break;
        }
        options_length += (size_t)_result;
    }

    // Inietta i valori attuali nel template HTML
    snprintf(_resp_str, BUFFER_SIZE, g_form_template, 
             g_device_config.ping_ip, 
             g_device_config.ping_host,
             login_options
            );

    // Invia la pagina popolata
    /* HTTPD_RESP_USE_STRLEN delega al g_server il calcolo della lunghezza fino
     * al terminatore NUL, evitando di mantenere un secondo contatore. */
    httpd_resp_send(req, _resp_str, HTTPD_RESP_USE_STRLEN);
    
    // Libera la memoria dinamica
    free(login_options);
    free(_resp_str);
    return ESP_OK;
}

// Handler per parametri "Hot" (Applicazione immediata)
static esp_err_t form_hot_handler(httpd_req_t *req) {
    /* I parametri "hot" modificano subito i target diagnostici e non
     * richiedono il riavvio della rete. */
    char buf[256] = {0};

    if (!receive_request_body(req, buf, sizeof(buf))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Richiesta non valida");
        return ESP_FAIL;
    }
    
    // AZZERA PRIMA DI SCRIVERE: garantisce che i vecchi caratteri non restino lì
    memset(g_device_config.ping_ip, 0, sizeof(g_device_config.ping_ip));
    memset(g_device_config.ping_host, 0, sizeof(g_device_config.ping_host));

    // Aggiorna la struttura globale in RAM
    /* Il body di un form URL-encoded ha la stessa forma di una query string;
     * questa API estrae per nome i valori nei buffer con limite di capacità. */
    httpd_query_key_value(buf, "ping_ip", g_device_config.ping_ip, sizeof(g_device_config.ping_ip));
    httpd_query_key_value(buf, "ping_host", g_device_config.ping_host, sizeof(g_device_config.ping_host));

    url_decode(g_device_config.ping_ip);
    url_decode(g_device_config.ping_host);

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
    /* Si lavora su una copia: la configurazione globale resta coerente fino
     * al salvataggio e al successivo riavvio. */
    char buf[256] = {0};
    if (!receive_request_body(req, buf, sizeof(buf))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Richiesta non valida");
        return ESP_FAIL;
    }

    app_config_t config;
    memcpy(&config, &g_device_config, sizeof(app_config_t));
    char login_ixp_string[16] = "-1";
    httpd_query_key_value(buf, "login_ixp", login_ixp_string, sizeof(login_ixp_string));

    char *end = NULL;
    /* strtol consente di distinguere un indice numerico valido da input
     * malformato controllando sia end sia l'eventuale testo residuo. */
    const long requested_ixp = strtol(login_ixp_string, &end, 10);
    if (end == login_ixp_string || *end != '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Indice hotspot non valido");
        return ESP_FAIL;
    }

    if (requested_ixp >= 0) {
        if (!login_save_set_current(&config.wifi_logins, (size_t)requested_ixp)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Hotspot non disponibile");
            return ESP_FAIL;
        }
    } else {
        wifi_login_t new_login = {0};
        httpd_query_key_value(buf, "ssid", new_login.ssid, sizeof(new_login.ssid));
        httpd_query_key_value(buf, "pass", new_login.password, sizeof(new_login.password));

        // Decodifica la stringa da percent-encoding del form HTML (es. %21 -> !, %27 -> ', + -> spazio)
        url_decode(new_login.ssid);
        url_decode(new_login.password);

        if (new_login.ssid[0] == '\0') {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Inserire il nuovo SSID");
            return ESP_FAIL;
        }

        login_save_add(&config.wifi_logins, &new_login);
        login_save_set_current(&config.wifi_logins,
                               login_save_length(&config.wifi_logins) - 1U);
    }

    // Salva nella memoria NVS
    config_save(&config);    
    httpd_resp_send(req, "<h2>Salvato. Riavvio in corso...</h2>", HTTPD_RESP_USE_STRLEN);
    
    /* Lascia tempo allo stack TCP di trasmettere la risposta prima del reset. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}


static esp_err_t favicon_get_handler(httpd_req_t *req) {
    // Specifichiamo al browser che è un'immagine vettoriale SVG
    httpd_resp_set_type(req, "image/svg+xml");
    
    // Inviamo la stringa usando strlen visto che è testo puro
    httpd_resp_send(req, g_favicon_svg, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler per mostrare la tabella dei dati diagnostici
static esp_err_t diag_page_handler(httpd_req_t *req) {
    // Allocazione dinamica per contenere la pagina HTML con la tabella
    // 2048 o più a seconda di quanti tag HTML vuoi inserire
    
    char *_resp_str = malloc(BUFFER_SIZE);
    if (_resp_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Inizio pagina HTML con lo script di refresh automatico sul click
    int len = snprintf(_resp_str, BUFFER_SIZE,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Diagnostica</title>"
        "<style>body{font-family:sans-serif;margin:20px;} table{width:100%%;border-collapse:collapse;} "
        "th,td{border:1px solid #ccc;padding:8px;text-align:left;} th{background:#f2f2f2;}</style></head>"
        "<body>"
        "<h2>Registro Diagnostica</h2>"
        // IL TRUCCO: Al click, avvia il download e dopo 500ms ricarica la pagina corrente
        "<button style='padding:10px;margin-bottom:15px;cursor:pointer;'"
        " onclick=\"location.href='/diag/download';"
        " setTimeout(function(){ location.reload(); }, 500);\">"
        " Scarica CSV e Svuota"
        "</button>"
        " | <a href='/params'>Params</a>"
        "<br>");
        
    /* Si mostra solo una finestra recente, mentre il download CSV comprende
     * l'intero contenuto del registro circolare. */
    uint16_t count = diag_get_count();
    len += snprintf(_resp_str+len, BUFFER_SIZE - len, 
        "<br>Count=%d (show last 10 events only)<br><br><table><tr><th>#</th><th>Timestamp</th><th>Error Mask (Bin)</th></tr>", count); 
    if (count == 0) {
        len += snprintf(_resp_str + len, BUFFER_SIZE - len, "<tr><td colspan='3'>Nessun dato registrato.</td></tr>");
    } else {
        const uint16_t _start = count > 10 ? count - 10 : 0; // massimo 10 righe
        for (uint16_t i = _start; i < count; i++) {
            DiagnosisEntry entry;
            if (diag_extract(i, &entry)) {
                if (BUFFER_SIZE - len < 150) break; // Margine di sicurezza aumentato per le stringhe più lunghe
                
                // 1. Conversione del Timestamp
                char time_buf[24];
                time_t t = (time_t)entry.timestamp; // Se è in millisecondi, fai: entry.timestamp / 1000;
                /* localtime applica il fuso configurato da SNTP; strftime
                 * serializza poi l'orario nel formato usato dalla tabella. */
                struct tm *tm_info = localtime(&t);
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%m-%S", tm_info);

                // 2. Conversione dell'Error Mask (prendiamo solo gli 8 bit meno significativi)
                char bin_buf[9];
                byte_to_binary_str((uint8_t)(entry.error_mask & 0xFF), bin_buf);
                
                len += snprintf(_resp_str + len, BUFFER_SIZE - len,
                    "<tr><td>%d</td><td>%s</td><td><code>b%s</code></td></tr>",
                    i + 1, time_buf, bin_buf);
            }
        }
    }

    // Chiusura tag HTML
    snprintf(_resp_str + len, BUFFER_SIZE - len, "</table></body></html>");

    /* Il Content-Type permette al browser di interpretare la risposta come HTML. */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, _resp_str, HTTPD_RESP_USE_STRLEN);
    
    free(_resp_str);
    return ESP_OK;
}

// Handler per il download del CSV e successivo wipe del buffer
static esp_err_t diag_download_csv_handler(httpd_req_t *req) {
    // Allochiamo un buffer temporaneo per chunk di testo o per l'intero file.
    // Con 64 entry massime, il CSV intero occuperà circa 1.5 - 2 KB.
    const int DOWNLOAD_BUFFER_SIZE = 2048; // = 32byte * 64;
    char *csv_buf = malloc(DOWNLOAD_BUFFER_SIZE);
    if (csv_buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Forza il browser a scaricare il contenuto come file invece di visualizzarlo
    httpd_resp_set_type(req, "text/csv");
    /* Content-Disposition attachment chiede al browser di proporre il file
     * come download usando il nome indicato. */
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=diagnostica_cerbero.csv");

    int len = snprintf(csv_buf, DOWNLOAD_BUFFER_SIZE, "Indice;Timestamp;ErrorMask\n");

    uint16_t count = diag_get_count();
    for (uint16_t i = 0; i < count; i++) {
        DiagnosisEntry entry;
        if (diag_extract(i, &entry)) {
            if (DOWNLOAD_BUFFER_SIZE - len < 80) break;

            // 1. Conversione del Timestamp
            char time_buf[24];
            time_t t = (time_t)entry.timestamp; // Se è in millisecondi: entry.timestamp / 1000;
            struct tm *tm_info = localtime(&t);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d_%H-%m-%S", tm_info);

            // 2. Conversione dell'Error Mask
            char bin_buf[9];
            byte_to_binary_str((uint8_t)(entry.error_mask & 0xFF), bin_buf);

            len += snprintf(csv_buf + len, DOWNLOAD_BUFFER_SIZE - len, "%d;%s;b%s\n",
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
    /* HTTPD_DEFAULT_CONFIG fornisce porta, priorità e limiti standard. Lo
     * stack viene ampliato perché gli handler formattano pagine con newlib. */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    
    // Inizializza e avvia il g_server
    if (httpd_start(&g_server, &config) == ESP_OK) {
        /* Ogni httpd_uri_t associa metodo + percorso a una callback handler;
         * user_ctx resta NULL perché gli handler usano lo stato globale. */
        httpd_uri_t uri_get = {
            .uri       = "/params",
            .method    = HTTP_GET,
            .handler   = form_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_server, &uri_get);

        httpd_uri_t uri_hot = {
            .uri       = "/submit_hot",
            .method    = HTTP_POST,
            .handler   = form_hot_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_server, &uri_hot);

        httpd_uri_t uri_cold = {
            .uri       = "/submit_cold",
            .method    = HTTP_POST,
            .handler   = form_cold_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_server, &uri_cold);

        httpd_uri_t uri_favicon = {
            .uri       = "/favicon.ico",
            .method    = HTTP_GET,
            .handler   = favicon_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_server, &uri_favicon);

        // URI per la visualizzazione della Tabella Diagnostica
        httpd_uri_t uri_diag = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = diag_page_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_server, &uri_diag);

        // URI per il download del CSV
        httpd_uri_t uri_diag_down = {
            .uri       = "/diag/download",
            .method    = HTTP_GET,
            .handler   = diag_download_csv_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(g_server, &uri_diag_down);

        ESP_LOGI(TAG, "Server HTTP avviato");
        return ESP_OK;
    }
    return ESP_FAIL;
}

void stop_webserver(void) {
    if (g_server) {
        /* httpd_stop arresta il task interno e invalida l'handle conservato. */
        httpd_stop(g_server);
        g_server = NULL;
    }
}
