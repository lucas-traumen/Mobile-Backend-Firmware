idf_component_register(SRCS "broker_uri.c" "mqtt_app.c"
                       INCLUDE_DIRS "include"
                       # esp-mqtt comes from the managed component (main/idf_component.yml).
                       # Reference it by its namespaced component name — do NOT add
                       # a REQUIRES to the IDF "mqtt" component (removed in IDF v6).
                       # cJSON from the registry parses relay command payloads.
                       PRIV_REQUIRES espressif__mqtt relay espressif__cjson)
