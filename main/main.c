#include <stdio.h>
#include <string.h>
// Librerie core di FreeRTOS per la gestione di task, code, semafori ed eventi
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
// Librerie specifiche ESP-IDF per l'hardware e il networking
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
// Librerie lwIP (Lightweight IP) per lo stack di rete TCP/IP e DNS
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
// Componenti custom/di sistema del tuo template
#include "system_utils.h"
#include "sdkconfig.h"
#include "driver/ledc.h" // Usato per il buzzer.
#include "param_persist.h"
#include "params_http.h"
#include "sntp.h"


#define AP_SSID "ESP32_Network-Cerbero"
#define AP_PASS "netcer1357"

// Variabile globale che conterrà i parametri di rete
app_config_t device_config;

// Definizione del pin del Buzzer
#define BUZZER_PIN 3

// Definizione del pin del pulsante per forzare la modalita' Access Point (AP)
// Collegato a GPIO 2, normalmente aperto, attivo a livello basso (0)
#define FORCE_AP_BUTTON_PIN 2

// Tag utilizzato per filtrare i log sulla seriale
static const char *TAG = "main";

// Definizione del pin del LED, recuperato dalla configurazione del progetto (menuconfig)
#define DIAG_LED_PIN 8 //CONFIG_BLINK_GPIO_PIN = 2!

// Struttura dati che definisce il "comando" di lampeggio
typedef struct {
    uint32_t period_on_ms;  // Tempo di accensione in millisecondi
    uint32_t period_off_ms; // Tempo di spegnimento in millisecondi
    uint32_t duration_ms;   // Durata totale dell'animazione
    bool enable_buzzer;     // Abilita/disabilita il buzzer
} led_blink_cmd_t;

// Handle per la coda di messaggi usata per comunicare con il task del LED
static QueueHandle_t led_cmd_queue = NULL;

// Funzione helper per inviare un nuovo comando di lampeggio al task del LED
void led_blink_start(uint32_t period_on_ms, uint32_t period_off_ms, uint32_t duration_ms, bool use_buzzer) {
    led_blink_cmd_t cmd = {
        .period_on_ms = period_on_ms,
        .period_off_ms = period_off_ms,
        .duration_ms = duration_ms,
        .enable_buzzer = use_buzzer
    };
    // xQueueOverwrite sovrascrive l'ultimo messaggio se la coda è piena (in questo caso è di 1 solo elemento).
    // Questo permette di interrompere un lampeggio in corso con uno nuovo senza blocchi.
    if (led_cmd_queue != NULL) {
        xQueueOverwrite(led_cmd_queue, &cmd);
    }
}

void buzzer_init(void) {
    // 1. Configura il timer PWM
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT, // Risoluzione a 13 bit (valori da 0 a 8191)
        .freq_hz          = 4150,              // Frequenza dell'onda quadra (2 kHz)
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 2. Collega il timer al pin del buzzer
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = BUZZER_PIN,
        .duty           = 0, // Parte con duty cycle a 0 (spento)
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

void buzzer_set_state(bool on) {
    if (on) {
        // Accende il suono: duty cycle al 50% di 8192 = 4096
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4096);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else {
        // Spegne il suono: duty cycle allo 0%
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

// Task FreeRTOS indipendente che gestisce il lampeggio del LED senza bloccare il resto del codice
static void led_blink_task(void *pvParameters) {
    led_blink_cmd_t current_cmd = {0}; // inizializza tutti i campi a zero.
    bool is_blinking = false;
    TickType_t start_tick = 0; // Memorizza i tick di sistema all'inizio del lampeggio

    // Configurazione del registro hardware del GPIO per il LED
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DIAG_LED_PIN), // Seleziona il pin
        .mode = GPIO_MODE_OUTPUT,               // Imposta come output
        .pull_up_en = GPIO_PULLUP_DISABLE,      // Niente resistenze di pull interne
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,         // Nessun interrupt abilitato
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf)); // Applica la configurazione e blocca in caso di errore

    // Inizializza il generatore PWM per il buzzer
    buzzer_init();

    // Stato di riposo iniziale: LED SPENTO. Logica Active-Low: 1 = spento.
    gpio_set_level(DIAG_LED_PIN, 1);
    buzzer_set_state(false);

    while (1) {
        led_blink_cmd_t new_cmd;
        // Se non stiamo lampeggiando, blocca il task all'infinito (portMAX_DELAY) in attesa di un comando.
        // Se stiamo lampeggiando, non bloccare (0) così possiamo continuare l'animazione.
        TickType_t wait_ticks = is_blinking ? 0 : portMAX_DELAY;

        // Controlla se è arrivato un nuovo comando nella coda
        if (xQueueReceive(led_cmd_queue, &new_cmd, wait_ticks) == pdPASS) {
            current_cmd = new_cmd;
            is_blinking = (current_cmd.duration_ms > 0);
            start_tick = xTaskGetTickCount(); // Salva il momento di inizio comando
            
            if (is_blinking) {
                gpio_set_level(DIAG_LED_PIN, 0); // Inizia accendendo il LED (LOW = 0)
                buzzer_set_state(current_cmd.enable_buzzer); // BUZZER ON SE RICHIESTO
            } else {
                gpio_set_level(DIAG_LED_PIN, 1); // Spegni il LED (HIGH = 1)
                buzzer_set_state(false);         // BUZZER OFF
            }
        }

        // Macchina a stati per gestire l'animazione del lampeggio
        if (is_blinking) {
            // Calcola da quanti millisecondi sta andando avanti questo comando
            uint32_t elapsed_ms = (xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;
            
            if (elapsed_ms >= current_cmd.duration_ms) {
                // Il tempo totale (es. 5 secondi di errore) è scaduto: forza lo spegnimento
                gpio_set_level(DIAG_LED_PIN, 1);
                buzzer_set_state(false);         // BUZZER OFF
                is_blinking = false;
            } else {
                // Calcola in che fase del ciclo ON/OFF ci troviamo usando l'operatore modulo
                uint32_t period = current_cmd.period_on_ms + current_cmd.period_off_ms;
                if (period > 0) {
                    uint32_t phase = elapsed_ms % period;
                    if (phase < current_cmd.period_on_ms) {
                        gpio_set_level(DIAG_LED_PIN, 0); // Fase ON (LOW)
                        buzzer_set_state(current_cmd.enable_buzzer); // BUZZER ON SE RICHIESTO
                    } else {
                        gpio_set_level(DIAG_LED_PIN, 1); // Fase OFF (HIGH)
                        buzzer_set_state(false);         // BUZZER OFF
                    }
                }
                // Rilascia la CPU per 10ms ad altri task prima di ricalcolare la fase
                vTaskDelay(pdMS_TO_TICKS(10)); 
            }
        }
    }
}

// Strutture per la gestione degli eventi Wi-Fi
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0 // Bit flag usato per segnalare l'avvenuta connessione

// Callback invocata in background da ESP-IDF quando cambia lo stato del Wi-Fi o dell'IP
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started. Connecting...");
        esp_wifi_connect(); // Avvia la connessione all'Access Point
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected. Retrying connection...");
        // Azzera il bit di connessione nel gruppo eventi
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect(); // Riprova a connettersi in automatico
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Quando il DHCP assegna l'IP, estrae l'indirizzo dalla struttura dati e lo logga
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Inizializza il servizio SNTP:
        initialize_sntp(TAG);
        // Imposta il bit per sbloccare il task di diagnostica in attesa
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    // Configura il pin per il pulsante Normalmente Aperto per forzare l'Access Point.
    // Il pulsante chiude verso massa (GND) quindi usiamo PULLUP interno. Stato premuto = 0 (LOW).
    gpio_config_t io_conf_btn = {
        .pin_bit_mask = (1ULL << FORCE_AP_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf_btn));

    // Ritardo di stabilizzazione del pin prima della lettura per evitare transienti di accensione
    vTaskDelay(pdMS_TO_TICKS(50));

    bool force_ap = false;
    // Se all'avvio il pulsante è premuto (livello logico LOW/0)
    if (gpio_get_level(FORCE_AP_BUTTON_PIN) == 0) {
        ESP_LOGI(TAG, "Pulsante premuto all'avvio. Avvio del test di 3 secondi...");

        // Inizializza il buzzer e attiva il beep continuo
        buzzer_init();
        buzzer_set_state(true);

        bool released = false;
        // Campiona il pulsante ogni 100 ms per un totale di 3 secondi (30 iterazioni)
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            // Se in qualsiasi momento il pulsante viene rilasciato (livello logico HIGH/1)
            if (gpio_get_level(FORCE_AP_BUTTON_PIN) != 0) {
                released = true;
                break;
            }
        }

        // Spegne il buzzer dopo il test
        buzzer_set_state(false);

        if (!released) {
            force_ap = true;
            ESP_LOGW(TAG, "Pulsante su GPIO %d tenuto premuto per 3 secondi. Forza modalita' Access Point!", FORCE_AP_BUTTON_PIN);
        } else {
            ESP_LOGI(TAG, "Pulsante su GPIO %d rilasciato prima dei 3 secondi. Avvio normale in corso.", FORCE_AP_BUTTON_PIN);
        }
    } else {
        ESP_LOGI(TAG, "Pulsante su GPIO %d non premuto all'avvio.", FORCE_AP_BUTTON_PIN);
    }

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init()); 
    ESP_ERROR_CHECK(esp_event_loop_create_default()); 

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Registra gli handler per gli eventi
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    if (force_ap || strlen(device_config.wifi_ssid) == 0) {
        if (force_ap) {
            ESP_LOGW(TAG, "Avvio forzato in modalita' Access Point via hardware.");
        } else {
            ESP_LOGW(TAG, "Nessun SSID configurato. Avvio in modalita' Access Point.");
        }
        
        // Interfaccia virtuale dedicata all'AP
        esp_netif_create_default_wifi_ap();

        wifi_config_t ap_config = {
            .ap = {
                .ssid = AP_SSID,
                .ssid_len = strlen(AP_SSID),
                .channel = 1,
                .password = AP_PASS,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK
            },
        };

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        start_webserver();
        ESP_LOGI(TAG, "Access Point avviato. Collegati a '%s' e apri 192.168.4.1", AP_SSID);
        
    } else {
        ESP_LOGI(TAG, "SSID trovato: %s. Avvio in modalita' Station.", device_config.wifi_ssid);

        // Interfaccia virtuale dedicata alla Station
        esp_netif_create_default_wifi_sta(); 

        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        
        strncpy((char *)wifi_config.sta.ssid, device_config.wifi_ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, device_config.wifi_password, sizeof(wifi_config.sta.password));        

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); 
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start()); 
        
        start_webserver(); 

        ESP_LOGI(TAG, "WiFi station initialization completed. Tentativo di connessione in corso...");
    }
}

// Struttura dati per incapsulare lo stato di un ping asincrono
typedef struct {
    SemaphoreHandle_t sem; // Semaforo usato per bloccare l'esecuzione in attesa dell'esito
    bool success;          // Flag di esito del ping
} ping_ctx_t;

// Callback asincrona chiamata dallo stack lwIP se arriva il pacchetto ICMP di risposta (ECHO REPLY)
static void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args) {
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    ctx->success = true;
}

// Callback asincrona chiamata se il ping va in timeout
static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    // Non facciamo nulla, success rimane false
}

// Callback asincrona chiamata quando la sessione di ping è formalmente conclusa
static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args) {
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    xSemaphoreGive(ctx->sem); // Sblocca il semaforo, risvegliando la funzione chiamante
}

// Funzione wrapper per eseguire un ping e attendere il risultato in modo sincrono
bool perform_ping(const ip_addr_t *target_ip) {
    // Crea un contesto contenente un semaforo binario per trasformare 
    // l'API asincrona del ping in una chiamata sincrona
    ping_ctx_t ctx = {
        .sem = xSemaphoreCreateBinary(),
        .success = false
    };
    if (!ctx.sem) return false;

    // Configura i parametri del ping (inviamo 1 solo pacchetto con timeout di 1 secondo)
    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = *target_ip;
    ping_config.count = 1;
    ping_config.timeout_ms = 1000; 

    // Collega le funzioni di callback
    esp_ping_callbacks_t cbs = {
        .on_ping_success = cmd_ping_on_ping_success,
        .on_ping_timeout = cmd_ping_on_ping_timeout,
        .on_ping_end = cmd_ping_on_ping_end,
        .cb_args = &ctx // Passiamo il nostro contesto (con il semaforo) alle callback
    };

    esp_ping_handle_t ping_handle;
    // Crea la sessione di rete per il ping
    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(ctx.sem);
        return false;
    }

    esp_ping_start(ping_handle); // Lancia la richiesta ICMP in background

    // Il task si blocca qui in attesa che la callback chiami xSemaphoreGive.
    // Timeout di sicurezza di 1200ms (poco più del timeout del ping stesso) per evitare blocchi definitivi.
    if (xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(1200)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping semaphore take timeout");
    }

    // Pulizia delle risorse di rete e FreeRTOS
    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    vSemaphoreDelete(ctx.sem);

    return ctx.success; // Ritorna l'esito reale popolato dalle callback
}

bool get_router_ip(ip_addr_t *target_ip) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        // Controlla che la lettura vada a buon fine e che il gateway non sia vuoto
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.gw.addr != 0) {
            target_ip->type = IPADDR_TYPE_V4;
            target_ip->u_addr.ip4.addr = ip_info.gw.addr;
            return true;
        }
    }
    return false; // Gateway non ancora assegnato o rete giù
}

// Task principale che esegue la diagnostica ciclica ogni 10 secondi
static void diagnostics_task(void *pvParameters) {
    ESP_LOGI(TAG, "Network diagnostics task started. Waiting for WiFi connection...");
    
    // Ferma l'esecuzione di questo task finché la callback del Wi-Fi non imposta il bit WIFI_CONNECTED_BIT
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected. Starting periodic network checks.");

    while (1) {
        // Registra il tick di inizio ciclo per calcolare correttamente l'attesa dei 10 secondi finali
        TickType_t cycle_start = xTaskGetTickCount();

        // 1. Lampeggio iniziale di notifica per segnalare l'inizio del test (invia comando alla coda)
        ESP_LOGI(TAG, "--------------------------------------------------");
        print_current_time(TAG);
        ESP_LOGI(TAG, "Starting sequential diagnostics check...");
        led_blink_start(200, 200, 400, false); 
        vTaskDelay(pdMS_TO_TICKS(400)); // Aspetta che il lampeggio singolo finisca

        // 2. Controllo LAN: Risoluzione IP router da stringa a formato binario (ip4addr_aton)
        ip_addr_t router_ip;

        // Tenta di recuperare dinamicamente l'IP del gateway
        if (!get_router_ip(&router_ip)) {
            ESP_LOGE(TAG, "LAN Check FAILED: Impossibile determinare l'IP del router!");
            led_blink_start(100, 100, 5000, false); // Allarme veloce (o con parametro del buzzer se preferisci)
            
            TickType_t elapsed = xTaskGetTickCount() - cycle_start;
            int32_t delay_ms = 10000 - (elapsed * portTICK_PERIOD_MS);
            if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
            
            continue; // Interrompe l'iterazione e riparte
        }

        // Ora che abbiamo l'IP, lo stampiamo e facciamo il ping
        ESP_LOGI(TAG, "Check 1/3: LAN - Pinging router at " IPSTR "...", IP2STR(&router_ip.u_addr.ip4));
        
        if (!perform_ping(&router_ip)) { // Chiama il wrapper sincrono creato sopra
            ESP_LOGE(TAG, "LAN Check FAILED!");
            led_blink_start(100, 100, 5000, false); // Comando di lampeggio veloce
            
            // Gestione precisa del timing: calcola quanto tempo è passato dall'inizio del loop
            // e dormi solo per i millisecondi rimanenti a raggiungere i 10 secondi (10000ms).
            TickType_t elapsed = xTaskGetTickCount() - cycle_start;
            int32_t delay_ms = 10000 - (elapsed * portTICK_PERIOD_MS);
            if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
            
            continue; // Interrompe l'iterazione attuale e riparte dall'inizio del while(1)
        }
        ESP_LOGI(TAG, "LAN Check PASSED.");

        // 3. Controllo WAN: Stessa logica del controllo LAN, ma punta all'8.8.8.8
        const char* target_ip_str = (strlen(device_config.ping_ip) > 0) ? device_config.ping_ip : "8.8.8.8";
        ip_addr_t wan_ip;
        ip4addr_aton(target_ip_str, &wan_ip.u_addr.ip4); // ip4addr_aton("8.8.8.8", &wan_ip.u_addr.ip4);
        wan_ip.type = IPADDR_TYPE_V4;

        ESP_LOGI(TAG, "Check 2/3: WAN - Pinging DNS at %s...", target_ip_str);
        if (!perform_ping(&wan_ip)) {
            ESP_LOGE(TAG, "WAN Check FAILED!");
            led_blink_start(200, 200, 5000, true); // Comando di lampeggio medio
            
            // Calcolo compensato dell'attesa
            TickType_t elapsed = xTaskGetTickCount() - cycle_start;
            int32_t delay_ms = 10000 - (elapsed * portTICK_PERIOD_MS);
            if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));
            continue; // Torna su
        }
        ESP_LOGI(TAG, "WAN Check PASSED.");

        // Recupera l'host dall'NVS, con fallback di sicurezza se la stringa è vuota
        const char* target_host = (strlen(device_config.ping_host) > 0) ? device_config.ping_host : "google.it";

        // 4. Controllo DNS: Risolve l'hostname
        ESP_LOGI(TAG, "Check 3/3: DNS - Resolving name %s...", target_host);
        
        struct addrinfo hints = {
            .ai_family = AF_INET,       // Forza la ricerca di un IPv4
            .ai_socktype = SOCK_STREAM, // Tipo di socket (ininfluente per la sola risoluzione)
        };
        struct addrinfo *res = NULL;
        
        // Chiamata all'API POSIX di lwIP per convertire l'URL nell'IP binario.
        // È bloccante, interroga il server DNS configurato sul router (ricevuto via DHCP)
        int err = getaddrinfo(target_host, NULL, &hints, &res);
        bool dns_success = false;

        if (err == 0 && res != NULL) {
            // Estrae l'indirizzo IPv4 dalla struttura e lo converte in stringa per stamparlo
            struct in_addr *addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
            char ip_str[16];
            inet_ntoa_r(*addr, ip_str, sizeof(ip_str));
            ESP_LOGI(TAG, "Resolved %s to %s. Verifying reachability...", target_host, ip_str);

            // Costruisce la struttura ip_addr_t passandogli l'IP binario appena risolto
            ip_addr_t dns_target_ip;
            dns_target_ip.type = IPADDR_TYPE_V4;
            dns_target_ip.u_addr.ip4.addr = addr->s_addr;

            // Fila finale: pinge l'IP appena ottenuto dal server DNS
            if (perform_ping(&dns_target_ip)) {
                dns_success = true;
            } else {
                ESP_LOGE(TAG, "Ping to resolved %s IP address failed.", target_host);
            }
            freeaddrinfo(res); // Libera la memoria allocata internamente da getaddrinfo
        } else {
            ESP_LOGE(TAG, "DNS resolution of %s failed with code: %d", target_host, err);
        }

        if (!dns_success) {
            ESP_LOGE(TAG, "DNS Check FAILED!");
            led_blink_start(500, 500, 5000, true); // Comando di lampeggio lento
        } else {
            ESP_LOGI(TAG, "DNS Check PASSED. Network is fully operational.");
        }
        // Calcolo dell'attesa finale a fine ciclo, tenendo conto del tempo impiegato per i tre controlli
        TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        int32_t delay_ms = 10000 - (elapsed * portTICK_PERIOD_MS);
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms)); // Restante parte dei 10 secondi
        }
    }
}

// Entry point standard di ESP-IDF (su FreeRTOS corrisponde ad un task generato internamente all'avvio)
void app_main(void) {
    ESP_LOGI(TAG, "Starting sequential diagnostics on ESP32-C3...");

    // Imposta l'aggiornamento del tempo ogni ora (TODO: scablare):
    //esp_sntp_set_sync_interval(60*60*1000);
    esp_sntp_set_sync_interval(1*60*1000);

    // 1. Inizializza il file system NVS
    ESP_ERROR_CHECK(config_nvs_init());

    // 2. Carica la configurazione dalla memoria Flash
    config_load(&device_config);

    // Richiama il setup custom presente nel tuo template (system_utils.c)
    system_utils_init();

    // Crea la coda IPC (Inter-Process Communication) che permette ai task di parlarsi.
    // Dimensione: 1 solo elemento della grandezza della struct led_blink_cmd_t.
    led_cmd_queue = xQueueCreate(1, sizeof(led_blink_cmd_t));
    if (led_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED command queue.");
        return;
    }

    // Istanzia il task per il LED sul Core 0 (l'ESP32-C3 ha un solo core, quindi andrà sempre sullo 0).
    // Stack: 3072 bytes. Priorità: 5 (alta, per non fargli saltare il timing dei lampeggi)
    xTaskCreatePinnedToCore(led_blink_task, "led_blink_task", 3072, NULL, 5, NULL, 0);

    // Esegue il setup iniziale del Wi-Fi e avvia la connessione
    wifi_init_sta();

    // Istanzia il task per la diagnostica di rete. 
    // Stack: 4096 bytes (il networking necessita di più RAM). Priorità: 4 (leggermente inferiore al LED)
    xTaskCreatePinnedToCore(diagnostics_task, "diagnostics_task", 4096, NULL, 4, NULL, 0);
}