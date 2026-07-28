idf_component_register(
    SRCS "main.c" "net_eth.c" "port.c" "crsf.c" "crsf_pacer.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif esp_event esp_timer driver nvs_flash lwip
)
