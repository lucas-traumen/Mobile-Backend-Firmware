idf_component_register(SRCS "main.c" "board_id.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_driver_i2c
                                    esp_driver_gpio
                                    wifi_conn sht3x mqtt_app relay ble_prov)
