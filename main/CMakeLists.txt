idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES system_utils nvs_flash esp_wifi esp_event esp_netif lwip driver)

