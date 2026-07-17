#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <time.h>
#include <sys/param.h>
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
#include "event_log.h"

/* Questo modulo coordina quattro sottosistemi: configurazione persistente,
 * connettività Wi-Fi, diagnostica periodica e segnalazione LED/buzzer. */

// Credenziali dell'Access Point di configurazione usato come modalità di recupero.
#define AP_SSID "ESP32_Network-Cerbero"
#define AP_PASS "netcer1357"

// Variabile globale che conterrà i parametri di rete
/* Viene caricata dalla NVS all'avvio ed è condivisa con gli handler HTTP. */
app_config_t g_device_config;

// Periodo nominale tra l'inizio di due cicli diagnostici consecutivi.
#define LOOP_TIME_MS 10000

// 
// Tick del ciclo precedente, usato per rilevare ritardi anomali del task.
TickType_t g_prev_cycle_start = 0;

// Ultimo risultato registrato: evita di salvare eventi identici a ogni ciclo.
DiagnosisEntry g_prev_dignosis_entry = {0};

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
static QueueHandle_t g_led_cmd_queue = NULL;

// Funzione helper per inviare un nuovo comando di lampeggio al task del LED
void led_blink_start(uint32_t period_on_ms, uint32_t period_off_ms, uint32_t duration_ms, bool use_buzzer) {
    led_blink_cmd_t _cmd = {
        .period_on_ms = period_on_ms,
        .period_off_ms = period_off_ms,
        .duration_ms = duration_ms,
        .enable_buzzer = use_buzzer
    };
    // xQueueOverwrite sovrascrive l'ultimo messaggio se la coda è piena (in questo caso è di 1 solo elemento).
    // Questo permette di interrompere un lampeggio in corso con uno nuovo senza blocchi.
    if (g_led_cmd_queue != NULL) {
        xQueueOverwrite(g_led_cmd_queue, &_cmd);
    }
}

void buzzer_init(void) {
    // 1. Configura il timer PWM
    ledc_timer_config_t _ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT, // Risoluzione a 13 bit (valori da 0 a 8191)
        .freq_hz          = 4150,              // Frequenza dell'onda quadra (2 kHz)
        .clk_cfg          = LEDC_AUTO_CLK
    };
    /* ESP_ERROR_CHECK interrompe il programma con diagnostica se il driver
     * rifiuta la configurazione: senza PWM il feedback acustico non è valido. */
    ESP_ERROR_CHECK(ledc_timer_config(&_ledc_timer));

    // 2. Collega il timer al pin del buzzer
    ledc_channel_config_t _ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = BUZZER_PIN,
        .duty           = 0, // Parte con duty cycle a 0 (spento)
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&_ledc_channel));
}

void buzzer_set_state(bool on) {
    if (on) {
        // Accende il suono: duty cycle al 50% di 8192 = 4096
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4096);
        /* set_duty prepara il nuovo valore; update_duty lo applica all'hardware. */
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else {
        // Spegne il suono: duty cycle allo 0%
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

// Task FreeRTOS indipendente che gestisce il lampeggio del LED senza bloccare il resto del codice
static void led_blink_task(void *pvParameters) {
    led_blink_cmd_t _current_cmd = {0}; // inizializza tutti i campi a zero.
    bool _is_blinking = false;
    TickType_t _start_tick = 0; // Memorizza i tick di sistema all'inizio del lampeggio

    // Configurazione del registro hardware del GPIO per il LED
    gpio_config_t _io_conf = {
        .pin_bit_mask = (1ULL << DIAG_LED_PIN), // Seleziona il pin
        .mode = GPIO_MODE_OUTPUT,               // Imposta come output
        .pull_up_en = GPIO_PULLUP_DISABLE,      // Niente resistenze di pull interne
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,         // Nessun interrupt abilitato
    };
    ESP_ERROR_CHECK(gpio_config(&_io_conf)); // Applica la configurazione e blocca in caso di errore

    // Inizializza il generatore PWM per il buzzer
    buzzer_init();

    // Stato di riposo iniziale: LED SPENTO. Logica Active-Low: 1 = spento.
    gpio_set_level(DIAG_LED_PIN, 1);
    buzzer_set_state(false);

    while (1) {
        led_blink_cmd_t _new_cmd;
        // Se non stiamo lampeggiando, blocca il task all'infinito (portMAX_DELAY) in attesa di un comando.
        // Se stiamo lampeggiando, non bloccare (0) così possiamo continuare l'animazione.
        TickType_t _wait_ticks = _is_blinking ? 0 : portMAX_DELAY;

        // Controlla se è arrivato un nuovo comando nella coda (bloccante per un tempo massimo _wait_ticks):
        if (xQueueReceive(g_led_cmd_queue, &_new_cmd, _wait_ticks) == pdPASS) {
            // entra nell'if solo se c'era un messaggio nella coda (pdPASS) altrimenti no (errQUEUE_EMPTY).
            _current_cmd = _new_cmd;
            _is_blinking = (_current_cmd.duration_ms > 0);
            _start_tick = xTaskGetTickCount(); // Salva il momento di inizio comando
            
            if (_is_blinking) {
                gpio_set_level(DIAG_LED_PIN, 0); // Inizia accendendo il LED (LOW = 0)
                buzzer_set_state(_current_cmd.enable_buzzer); // BUZZER ON SE RICHIESTO
            } else {
                gpio_set_level(DIAG_LED_PIN, 1); // Spegni il LED (HIGH = 1)
                buzzer_set_state(false);         // BUZZER OFF
            }
        }

        // Macchina a stati per gestire l'animazione del lampeggio
        if (_is_blinking) {
            // Calcola da quanti millisecondi sta andando avanti questo comando
            uint32_t _elapsed_ms = (xTaskGetTickCount() - _start_tick) * portTICK_PERIOD_MS;
            
            if (_elapsed_ms >= _current_cmd.duration_ms) {
                // Il tempo totale (es. 5 secondi di errore) è scaduto: forza lo spegnimento
                gpio_set_level(DIAG_LED_PIN, 1);
                buzzer_set_state(false);         // BUZZER OFF
                _is_blinking = false;
            } else {
                // Calcola in che fase del ciclo ON/OFF ci troviamo usando l'operatore modulo
                uint32_t _period = _current_cmd.period_on_ms + _current_cmd.period_off_ms;
                if (_period > 0) {
                    uint32_t _phase = _elapsed_ms % _period;
                    if (_phase < _current_cmd.period_on_ms) {
                        gpio_set_level(DIAG_LED_PIN, 0); // Fase ON (LOW)
                        buzzer_set_state(_current_cmd.enable_buzzer); // BUZZER ON SE RICHIESTO
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
static EventGroupHandle_t g_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0 // Bit flag usato per segnalare l'avvenuta connessione

// Callback invocata in background da ESP-IDF quando cambia lo stato del Wi-Fi o dell'IP
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    /* event_base distingue la famiglia dell'evento, event_id il caso concreto;
     * event_data punta alla struttura specifica prevista da quell'evento. */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started. Connecting...");
        esp_wifi_connect(); // Avvia la connessione all'Access Point
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected. Retrying connection...");
        // Azzera il bit di connessione nel gruppo eventi
        xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect(); // Riprova a connettersi in automatico
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Quando il DHCP assegna l'IP, estrae l'indirizzo dalla struttura dati e lo logga
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Inizializza il servizio SNTP:
        initialize_sntp(TAG);
        // Imposta il bit per sbloccare il task di diagnostica in attesa
        xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    wifi_login_t _current_login = {0};
    const bool has_current_login = login_save_get_current(
        &g_device_config.wifi_logins,
        &_current_login
    );

    // Configura il pin per il pulsante Normalmente Aperto per forzare l'Access Point.
    // Il pulsante chiude verso massa (GND) quindi usiamo PULLUP interno. Stato premuto = 0 (LOW).
    gpio_config_t _io_conf_btn = {
        .pin_bit_mask = (1ULL << FORCE_AP_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&_io_conf_btn));

    // Ritardo di stabilizzazione del pin prima della lettura per evitare transienti di accensione
    vTaskDelay(pdMS_TO_TICKS(50));

    bool _force_ap = false;
    // Se all'avvio il pulsante è premuto (livello logico LOW/0)
    if (gpio_get_level(FORCE_AP_BUTTON_PIN) == 0) {
        ESP_LOGI(TAG, "Pulsante premuto all'avvio. Avvio del test di 3 secondi...");

        // Inizializza il buzzer e attiva il beep continuo
        buzzer_init();
        buzzer_set_state(true);

        bool _released = false;
        // Campiona il pulsante ogni 100 ms per un totale di 3 secondi (30 iterazioni)
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            // Se in qualsiasi momento il pulsante viene rilasciato (livello logico HIGH/1)
            if (gpio_get_level(FORCE_AP_BUTTON_PIN) != 0) {
                _released = true;
                break;
            }
        }

        // Spegne il buzzer dopo il test
        buzzer_set_state(false);

        if (!_released) {
            _force_ap = true;
            ESP_LOGW(TAG, "Pulsante su GPIO %d tenuto premuto per 3 secondi. Forza modalita' Access Point!", FORCE_AP_BUTTON_PIN);
        } else {
            ESP_LOGI(TAG, "Pulsante su GPIO %d rilasciato prima dei 3 secondi. Avvio normale in corso.", FORCE_AP_BUTTON_PIN);
        }
    } else {
        ESP_LOGI(TAG, "Pulsante su GPIO %d non premuto all'avvio.", FORCE_AP_BUTTON_PIN);
    }

    /* L'Event Group è il ponte tra le callback asincrone del Wi-Fi e il task
     * diagnostico, che attende il bit di connessione senza polling continuo. */
    g_wifi_event_group = xEventGroupCreate();

    /* Crea lo stack TCP/IP e il loop eventi predefinito richiesti dal driver. */
    ESP_ERROR_CHECK(esp_netif_init()); 
    ESP_ERROR_CHECK(esp_event_loop_create_default()); 

    /* WIFI_INIT_CONFIG_DEFAULT inizializza anche campi interni dipendenti dalla
     * versione ESP-IDF, evitando configurazioni parziali incompatibili. */
    wifi_init_config_t _cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&_cfg));

    // Registra gli handler per gli eventi
    esp_event_handler_instance_t _instance_any_id;
    esp_event_handler_instance_t _instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &_instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &_instance_got_ip));

    if (_force_ap || !has_current_login || strlen(_current_login.ssid) == 0) {
        if (_force_ap) {
            ESP_LOGW(TAG, "Avvio forzato in modalita' Access Point via hardware.");
        } else {
            ESP_LOGW(TAG, "Nessun SSID configurato. Avvio in modalita' Access Point.");
        }
        
        // Interfaccia virtuale dedicata all'AP (Access Point)
        esp_netif_create_default_wifi_ap();

        wifi_config_t _ap_config = {
            /* La union wifi_config_t usa il membro ap perché il driver verrà
             * impostato in WIFI_MODE_AP. */
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
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &_ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        
        start_webserver();
        ESP_LOGI(TAG, "Access Point avviato. Collegati a '%s' e apri 192.168.4.1", AP_SSID);
        
    } else {
        ESP_LOGI(TAG, "SSID trovato: %s. Avvio in modalita' Station.", _current_login.ssid);

        // Interfaccia virtuale dedicata alla Station
        esp_netif_create_default_wifi_sta(); 

        wifi_config_t _wifi_config = {
            /* In modalità Station il membro sta contiene credenziali e soglia
             * minima di sicurezza accettata durante l'associazione. */
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        
        strncpy((char *)_wifi_config.sta.ssid, _current_login.ssid, sizeof(_wifi_config.sta.ssid));
        strncpy((char *)_wifi_config.sta.password, _current_login.password, sizeof(_wifi_config.sta.password));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); 
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &_wifi_config));
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
    ping_ctx_t *_ctx = (ping_ctx_t *)args;
    _ctx->success = true;
}

// Callback asincrona chiamata se il ping va in timeout
static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args) {
    // Non facciamo nulla, success rimane false
}

// Callback asincrona chiamata quando la sessione di ping è formalmente conclusa
static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args) {
    ping_ctx_t *_ctx = (ping_ctx_t *)args;
    xSemaphoreGive(_ctx->sem); // Sblocca il semaforo, risvegliando la funzione chiamante
}

// Funzione wrapper per eseguire un ping e attendere il risultato in modo sincrono
bool perform_ping(const ip_addr_t *target_ip) {
    // Crea un contesto contenente un semaforo binario per trasformare 
    // l'API asincrona del ping in una chiamata sincrona
    ping_ctx_t _ctx = {
        .sem = xSemaphoreCreateBinary(),
        .success = false
    };
    if (!_ctx.sem) return false;

    // Configura i parametri del ping (inviamo 1 solo pacchetto con timeout di 1 secondo)
    esp_ping_config_t _ping_config = ESP_PING_DEFAULT_CONFIG();
    _ping_config.target_addr = *target_ip;
    _ping_config.count = 1;
    _ping_config.timeout_ms = 1000; 

    // Collega le funzioni di callback
    esp_ping_callbacks_t _cbs = {
        .on_ping_success = cmd_ping_on_ping_success,
        .on_ping_timeout = cmd_ping_on_ping_timeout,
        .on_ping_end = cmd_ping_on_ping_end,
        .cb_args = &_ctx // Passiamo il nostro contesto (con il semaforo) alle callback
    };

    esp_ping_handle_t _ping_handle;
    // Crea la sessione di rete per il ping
    esp_err_t _err = esp_ping_new_session(&_ping_config, &_cbs, &_ping_handle);
    if (_err != ESP_OK) {
        vSemaphoreDelete(_ctx.sem);
        return false;
    }

    esp_ping_start(_ping_handle); // Lancia la richiesta ICMP in background

    // Il task si blocca qui in attesa che la callback chiami xSemaphoreGive.
    // Timeout di sicurezza di 1200ms (poco più del timeout del ping stesso) per evitare blocchi definitivi.
    if (xSemaphoreTake(_ctx.sem, pdMS_TO_TICKS(1200)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping semaphore take timeout");
    }

    // Pulizia delle risorse di rete e FreeRTOS
    esp_ping_stop(_ping_handle);
    esp_ping_delete_session(_ping_handle);
    vSemaphoreDelete(_ctx.sem);

    return _ctx.success; // Ritorna l'esito reale popolato dalle callback
}

bool get_router_ip(ip_addr_t *target_ip) {
    /* La chiave WIFI_STA_DEF identifica la _netif Station creata da
     * esp_netif_create_default_wifi_sta. Il gateway arriva normalmente via DHCP. */
    esp_netif_t *_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (_netif != NULL) {
        esp_netif_ip_info_t ip_info;
        // Controlla che la lettura vada a buon fine e che il gateway non sia vuoto
        if (esp_netif_get_ip_info(_netif, &ip_info) == ESP_OK && ip_info.gw.addr != 0) {
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
    xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected. Starting periodic network checks.");

    // Attesa non bloccante basata sulla time() standard
    {
        const time_t PRIMO_GENNAIO_2000 = 946684800; // 946684800 sono i secondi passati dal 1970 al 1 Gennaio 2000
        const int LOOP_STEP_WAIT_MS = 1000; // 1 secondo;
        const int LOOP_TOTAL_WAIT = 10; // 10 secondi;
        int _ntp_retry = 0;
        time_t _now;
        time(&_now);

        if (_now >= PRIMO_GENNAIO_2000) {
            ESP_LOGI(TAG, "Orario valido già rilevato.");
        }
        else{
            ESP_LOGI(TAG, "Attesa dell'orario sincronizzato (SNTP) ... ");
            // 946684800 sono i secondi passati dal 1970 al 1 Gennaio 2000
            while (_now < PRIMO_GENNAIO_2000 && _ntp_retry < LOOP_TOTAL_WAIT) {
                _ntp_retry++;
                vTaskDelay(pdMS_TO_TICKS(LOOP_STEP_WAIT_MS)); // Mette il task in pausa per 1 secondo, lasciando libera la CPU
                time(&_now);                      // Aggiorna il valore del tempo attuale
                ESP_LOGI(TAG, " .. ");
            }
        }

        if (_now >= PRIMO_GENNAIO_2000) {
            ESP_LOGI(TAG, "Orario valido rilevato. Avvio diagnostica periodica.");
        } else {
            ESP_LOGW(TAG, "Timeout NTP raggiunto. Procedo comunque con l'orologio non sincronizzato.");
        }
    }

    while (1) {
        // Registra il tick di inizio ciclo per calcolare correttamente l'attesa dei 10 secondi finali
        TickType_t _cycle_start = xTaskGetTickCount();

        // 1. Lampeggio iniziale di notifica per segnalare l'inizio del test (invia comando alla coda)
        ESP_LOGI(TAG, "--------------------------------------------------");
        DiagnosisEntry _dignosis_entry ={0};
        _dignosis_entry.error_mask |= 1;
        time(&_dignosis_entry.timestamp);
        print_time(TAG, &_dignosis_entry.timestamp);
        ESP_LOGI(TAG, "Starting sequential diagnostics check...");
        led_blink_start(200, 200, 400, false); 
        vTaskDelay(pdMS_TO_TICKS(400)); // Aspetta che il lampeggio singolo finisca

        // 2. Controllo LAN: Risoluzione IP router da stringa a formato binario (ip4addr_aton)
        ip_addr_t _router_ip;

        bool _test_continue = true;
        {
            uint32_t _elapsed_ms = (_cycle_start - g_prev_cycle_start) * portTICK_PERIOD_MS;
            ESP_LOGI(TAG, "DBG: _elapsed_ms = %" PRIu32, _elapsed_ms);
            g_prev_cycle_start = _cycle_start;
            if (_elapsed_ms > LOOP_TIME_MS) { 
                _dignosis_entry.error_mask |= 2;
            }
        }

        // Tenta di recuperare dinamicamente l'IP del gateway
        if (!get_router_ip(&_router_ip)) {
            ESP_LOGE(TAG, "LAN Check FAILED: Impossibile determinare l'IP del router!");
            led_blink_start(100, 100, 5000, false); // Allarme veloce (o con parametro del buzzer se preferisci)
            
            // TickType_t elapsed = xTaskGetTickCount() - _cycle_start;
            // int32_t _delay_ms = LOOP_TIME_MS - (elapsed * portTICK_PERIOD_MS);
            // if (_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(_delay_ms));
            
            _test_continue = false;
            _dignosis_entry.error_mask |= 4; // Imposta il bit di errore
            //continue; // Interrompe l'iterazione e riparte
        }

        if (_test_continue) {
            // Ora che abbiamo l'IP, lo stampiamo e facciamo il ping
            ESP_LOGI(TAG, "Check 1/3: LAN - Pinging router at " IPSTR "...", IP2STR(&_router_ip.u_addr.ip4));
            
            if (!perform_ping(&_router_ip)) { // Chiama il wrapper sincrono creato sopra
                ESP_LOGE(TAG, "LAN Check FAILED!");
                led_blink_start(100, 100, 5000, false); // Comando di lampeggio veloce
                
                // // Gestione precisa del timing: calcola quanto tempo è passato dall'inizio del loop
                // // e dormi solo per i millisecondi rimanenti a raggiungere i 10 secondi (10000ms).
                // TickType_t elapsed = xTaskGetTickCount() - _cycle_start;
                // int32_t _delay_ms = LOOP_TIME_MS - (elapsed * portTICK_PERIOD_MS);
                // if (_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(_delay_ms));
                
                _dignosis_entry.error_mask |= 8; // Imposta il bit di errore
                _test_continue = false;
                //continue; // Interrompe l'iterazione attuale e riparte dall'inizio del while(1)
            }
            else {
                ESP_LOGI(TAG, "LAN Check PASSED.");
            }
        }

        if (_test_continue) {
            // 3. Controllo WAN: Stessa logica del controllo LAN, ma punta all'8.8.8.8
            const char* _target_ip_str = (strlen(g_device_config.ping_ip) > 0) ? g_device_config.ping_ip : "8.8.8.8";
            ip_addr_t _wan_ip;
            ip4addr_aton(_target_ip_str, &_wan_ip.u_addr.ip4); // ip4addr_aton("8.8.8.8", &_wan_ip.u_addr.ip4);
            _wan_ip.type = IPADDR_TYPE_V4;

            ESP_LOGI(TAG, "Check 2/3: WAN - Pinging DNS at %s...", _target_ip_str);
            if (!perform_ping(&_wan_ip)) {
                ESP_LOGE(TAG, "WAN Check FAILED!");
                led_blink_start(200, 200, 5000, true); // Comando di lampeggio medio
                
                // // Calcolo compensato dell'attesa
                // TickType_t elapsed = xTaskGetTickCount() - cycle_start;
                // int32_t _delay_ms = LOOP_TIME_MS - (elapsed * portTICK_PERIOD_MS);
                // if (_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(_delay_ms));
                _dignosis_entry.error_mask |= 16; // Imposta il bit di errore
                _test_continue = false;
                //continue; // Torna su
            }
            else {
                ESP_LOGI(TAG, "WAN Check PASSED.");
            }
        }

        if (_test_continue) {
            // Recupera l'host dall'NVS, con fallback di sicurezza se la stringa è vuota
            const char* _target_host = (strlen(g_device_config.ping_host) > 0) ? g_device_config.ping_host : "google.it";

            // 4. Controllo DNS: Risolve l'hostname
            ESP_LOGI(TAG, "Check 3/3: DNS - Resolving name %s...", _target_host);
            
            struct addrinfo _hints = {
                /* addrinfo descrive i vincoli della risoluzione e riceve da
                 * getaddrinfo una lista allocata dinamicamente di risultati. */
                .ai_family = AF_INET,       // Forza la ricerca di un IPv4
                .ai_socktype = SOCK_STREAM, // Tipo di socket (ininfluente per la sola risoluzione)
            };
            struct addrinfo *_res = NULL;
            
            // Chiamata all'API POSIX di lwIP per convertire l'URL nell'IP binario.
            // È bloccante, interroga il server DNS configurato sul router (ricevuto via DHCP)
            int _err = getaddrinfo(_target_host, NULL, &_hints, &_res);
            bool _dns_success = false;

            if (_err == 0 && _res != NULL) {
                // Estrae l'indirizzo IPv4 dalla struttura e lo converte in stringa per stamparlo
                struct in_addr *_addr = &((struct sockaddr_in *)_res->ai_addr)->sin_addr;
                char ip_str[16];
                inet_ntoa_r(*_addr, ip_str, sizeof(ip_str));
                ESP_LOGI(TAG, "Resolved %s to %s. Verifying reachability...", _target_host, ip_str);

                // Costruisce la struttura ip_addr_t passandogli l'IP binario appena risolto
                ip_addr_t _dns_target_ip;
                _dns_target_ip.type = IPADDR_TYPE_V4;
                _dns_target_ip.u_addr.ip4.addr = _addr->s_addr;

                // Fila finale: pinge l'IP appena ottenuto dal server DNS
                if (perform_ping(&_dns_target_ip)) {
                    _dns_success = true;
                } else {
                    ESP_LOGE(TAG, "Ping to resolved %s IP address failed.", _target_host);
                }
                freeaddrinfo(_res); // Libera la memoria allocata internamente da getaddrinfo
            } else {
                ESP_LOGE(TAG, "DNS resolution of %s failed with code: %d", _target_host, _err);
            }

            if (!_dns_success) {
                ESP_LOGE(TAG, "DNS Check FAILED!");
                _dignosis_entry.error_mask |= 32; // Imposta il bit di errore
                led_blink_start(500, 500, 5000, true); // Comando di lampeggio lento
                _test_continue = false;
            } else {
                ESP_LOGI(TAG, "DNS Check PASSED. Network is fully operational.");
            }
        }

        if (g_prev_dignosis_entry.error_mask != _dignosis_entry.error_mask) {
            ESP_LOGI(TAG, "DBG: >>>> ERROR MASK CHANGED!");
            bool _lost_data = diag_append(_dignosis_entry);
            if (_lost_data) {
                ESP_LOGW(TAG, "Buffer pieno! Sovrascrittura avvenuta, dati persi.");
            }
        }
        g_prev_dignosis_entry = _dignosis_entry;

        // Calcolo dell'attesa finale a fine ciclo, tenendo conto del tempo impiegato per i tre controlli
        TickType_t elapsed = xTaskGetTickCount() - _cycle_start;
        int32_t _delay_ms = LOOP_TIME_MS - (elapsed * portTICK_PERIOD_MS); 
        if (_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(_delay_ms)); // Restante parte dei 10 secondi
        }
    }
}

// Entry point standard di ESP-IDF (su FreeRTOS corrisponde ad un task generato internamente all'avvio)
void app_main(void) {
    ESP_LOGI(TAG, "Starting sequential diagnostics on ESP32-C3...");


    ESP_LOGI("DEBUG", "Forzatura dell'orologio di sistema a zero per test...");
    {
        struct timeval tv = {
            .tv_sec = 0,  // Imposta i secondi al 1 Gennaio 1970, ore 00:00:00
            .tv_usec = 0
        };
        settimeofday(&tv, NULL);
        
        // Configura anche la Timezone a UTC per i test, così localtime() non aggiungerà offset 
        // e sposterà l'orario a ore 01:00:00 a causa del fuso orario italiano
        setenv("TZ", "UTC0", 1);
        tzset();
    }

    // Imposta l'aggiornamento del tempo ogni ora (TODO: scablare):
    //esp_sntp_set_sync_interval(60*60*1000);
    esp_sntp_set_sync_interval(1*60*1000);

    // 1. Inizializza il file system NVS
    /* Un errore NVS rende inaffidabile tutta la configurazione: ESP_ERROR_CHECK
     * produce log e reset controllato anziché proseguire con stato indefinito. */
    ESP_ERROR_CHECK(config_nvs_init());

    // 2. Carica la configurazione dalla memoria Flash (nvs)
    config_load(&g_device_config);

    // Richiama il setup custom presente nel tuo template (system_utils.c)
    system_utils_init(); // attualmente fa solo un log.

    // Crea la coda IPC (Inter-Process Communication) che permette ai task di parlarsi.
    // Dimensione: 1 solo elemento della grandezza della struct led_blink_cmd_t.
    g_led_cmd_queue = xQueueCreate(1, sizeof(led_blink_cmd_t));
    if (g_led_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED command queue.");
        return;
    }

    // Istanzia il task per il LED sul Core 0 (l'ESP32-C3 ha un solo core, quindi andrà sempre sullo 0).
    // Stack: 3072 bytes. Priorità: 5 (alta, per non fargli saltare il timing dei lampeggi)
    /* Parametri: funzione, nome diagnostico, stack, argomento, priorità,
     * handle opzionale e core su cui fissare l'esecuzione. */
    xTaskCreatePinnedToCore(led_blink_task, "led_blink_task", 3072, NULL, 5, NULL, 0);

    // Esegue il setup iniziale del Wi-Fi e avvia la connessione
    wifi_init_sta();

    // Istanzia il task per la diagnostica di rete. 
    // Stack: 4096 bytes (il networking necessita di più RAM). Priorità: 4 (leggermente inferiore al LED)
    xTaskCreatePinnedToCore(diagnostics_task, "diagnostics_task", 4096, NULL, 4, NULL, 0);
}
