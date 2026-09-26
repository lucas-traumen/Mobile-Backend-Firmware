idf_component_register(SRCS "ble_prov.c"
                       INCLUDE_DIRS "include"
                       # NimBLE host (GATT server / GAP / SM) lives in the "bt"
                       # component; provisioned credentials persist in the
                       # "bleprov" NVS namespace (nvs_flash). esp_timer drives
                       # the auto-stop 30 s after CONNECTED. esp_log is part of
                       # the build system's common requirements (see relay/).
                       REQUIRES bt nvs_flash
                       PRIV_REQUIRES esp_timer)
