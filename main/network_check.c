#include "network_check.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

static const char *TAG = "NETWORK_CHECK";

// Struttura dati per incapsulare lo stato di un ping asincrono
typedef struct {
    SemaphoreHandle_t sem; // Semaforo usato per bloccare l'esecuzione in attesa dell'esito
    bool _success;          // Flag di esito del ping
} ping_ctx_t;

// Callback asincrona chiamata dallo stack lwIP se arriva il pacchetto ICMP di risposta (ECHO REPLY)
static void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *_ctx = (ping_ctx_t *)args;
    _ctx->_success = true;
}

// Callback asincrona chiamata se il ping va in timeout
static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    // Non facciamo nulla, _success rimane false
}

// Callback asincrona chiamata quando la sessione di ping è formalmente conclusa
static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *_ctx = (ping_ctx_t *)args;
    xSemaphoreGive(_ctx->sem); // Sblocca il semaforo, risvegliando la funzione chiamante
}

bool perform_ping(const ip_addr_t *target_ip)
{
    ping_ctx_t _ctx = {
        .sem = xSemaphoreCreateBinary(),
        ._success = false
    };
    if (!_ctx.sem) {
        return false;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = *target_ip;
    ping_config.count = 1;
    ping_config.timeout_ms = 1000;

    esp_ping_callbacks_t callbacks = {
        .on_ping_success = cmd_ping_on_ping_success,
        .on_ping_timeout = cmd_ping_on_ping_timeout,
        .on_ping_end = cmd_ping_on_ping_end,
        .cb_args = &_ctx
    };

    esp_ping_handle_t _ping_handle;
    esp_err_t _err = esp_ping_new_session(&ping_config, &callbacks, &_ping_handle);
    if (_err != ESP_OK) {
        vSemaphoreDelete(_ctx.sem);
        return false;
    }

    esp_ping_start(_ping_handle);
    if (xSemaphoreTake(_ctx.sem, pdMS_TO_TICKS(1200)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping semaphore take timeout");
    }

    esp_ping_stop(_ping_handle);
    esp_ping_delete_session(_ping_handle);
    vSemaphoreDelete(_ctx.sem);
    return _ctx._success;
}

bool get_router_ip(ip_addr_t *target_ip)
{
    /* La chiave WIFI_STA_DEF identifica la _netif Station creata da
     * esp_netif_create_default_wifi_sta. */
    esp_netif_t *_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (_netif != NULL) {
        esp_netif_ip_info_t _ip_info;
        if (esp_netif_get_ip_info(_netif, &_ip_info) == ESP_OK && _ip_info.gw.addr != 0) {
            target_ip->type = IPADDR_TYPE_V4;
            target_ip->u_addr.ip4.addr = _ip_info.gw.addr;
            return true;
        }
    }
    return false;
}

bool perform_dns_check(const char *target_host)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *_result = NULL;

    int _err = getaddrinfo(target_host, NULL, &hints, &_result);
    if (_err != 0 || _result == NULL) {
        ESP_LOGE(TAG, "DNS resolution of %s failed with code: %d", target_host, _err);
        return false;
    }

    struct in_addr *_address = &((struct sockaddr_in *)_result->ai_addr)->sin_addr;
    char _ip_string[16];
    inet_ntoa_r(*_address, _ip_string, sizeof(_ip_string));
    ESP_LOGI(TAG, "Resolved %s to %s. Verifying reachability...", target_host, _ip_string);

    ip_addr_t target_ip = {
        .u_addr.ip4.addr = _address->s_addr,
        .type = IPADDR_TYPE_V4,
    };
    bool _success = perform_ping(&target_ip);
    freeaddrinfo(_result);
    return _success;
}
