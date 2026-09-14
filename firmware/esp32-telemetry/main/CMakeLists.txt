idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_driver_i2c
                                    wifi_conn sht3x mqtt_app relay)
