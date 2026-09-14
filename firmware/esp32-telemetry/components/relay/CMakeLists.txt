idf_component_register(SRCS "relay.c"
                       INCLUDE_DIRS "include"
                       # Pure GPIO driver: no I2C / Wi-Fi / MQTT dependency.
                       PRIV_REQUIRES esp_driver_gpio)
