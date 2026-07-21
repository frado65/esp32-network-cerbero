idf_component_register(SRCS "display.c" "diagnostics.c" "wifi_manager.c" "network_check.c" "status_indicator.c" "sntp.c" "event_log.c" "main.c"
                            "params_http.c"
                            "param_persist.c"
                            "login_save.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES system_utils nvs_flash esp_wifi esp_event esp_netif lwip driver esp_http_server)
