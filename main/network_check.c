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
    bool success;          // Flag di esito del ping
} ping_ctx_t;

// Callback asincrona chiamata dallo stack lwIP se arriva il pacchetto ICMP di risposta (ECHO REPLY)
static void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    ctx->success = true;
}

// Callback asincrona chiamata se il ping va in timeout
static void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    // Non facciamo nulla, success rimane false
}

// Callback asincrona chiamata quando la sessione di ping è formalmente conclusa
static void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    xSemaphoreGive(ctx->sem); // Sblocca il semaforo, risvegliando la funzione chiamante
}

bool perform_ping(const ip_addr_t *target_ip)
{
    ping_ctx_t ctx = {
        .sem = xSemaphoreCreateBinary(),
        .success = false
    };
    if (!ctx.sem) {
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
        .cb_args = &ctx
    };

    esp_ping_handle_t ping_handle;
    esp_err_t err = esp_ping_new_session(&ping_config, &callbacks, &ping_handle);
    if (err != ESP_OK) {
        vSemaphoreDelete(ctx.sem);
        return false;
    }

    esp_ping_start(ping_handle);
    if (xSemaphoreTake(ctx.sem, pdMS_TO_TICKS(1200)) != pdTRUE) {
        ESP_LOGE(TAG, "Ping semaphore take timeout");
    }

    esp_ping_stop(ping_handle);
    esp_ping_delete_session(ping_handle);
    vSemaphoreDelete(ctx.sem);
    return ctx.success;
}

bool get_router_ip(ip_addr_t *target_ip)
{
    /* La chiave WIFI_STA_DEF identifica la netif Station creata da
     * esp_netif_create_default_wifi_sta. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.gw.addr != 0) {
            target_ip->type = IPADDR_TYPE_V4;
            target_ip->u_addr.ip4.addr = ip_info.gw.addr;
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
    struct addrinfo *result = NULL;

    int err = getaddrinfo(target_host, NULL, &hints, &result);
    if (err != 0 || result == NULL) {
        ESP_LOGE(TAG, "DNS resolution of %s failed with code: %d", target_host, err);
        return false;
    }

    struct in_addr *address = &((struct sockaddr_in *)result->ai_addr)->sin_addr;
    char ip_string[16];
    inet_ntoa_r(*address, ip_string, sizeof(ip_string));
    ESP_LOGI(TAG, "Resolved %s to %s. Verifying reachability...", target_host, ip_string);

    ip_addr_t target_ip = {
        .u_addr.ip4.addr = address->s_addr,
        .type = IPADDR_TYPE_V4,
    };
    bool success = perform_ping(&target_ip);
    freeaddrinfo(result);
    return success;
}
