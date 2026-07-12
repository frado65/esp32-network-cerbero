idf_component_register(SRCS "event_log.c" "main.c"
                            "params_http.c"
                            "param_persist.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES system_utils nvs_flash esp_wifi esp_event esp_netif lwip driver esp_http_server)
