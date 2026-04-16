idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES RCSwitch
                    PRIV_REQUIRES esp_driver_gpio
                    PRIV_REQUIRES esp_driver_ledc
                    PRIV_REQUIRES esp_wifi
                    PRIV_REQUIRES esp_event
                    PRIV_REQUIRES esp_netif
                    PRIV_REQUIRES esp_timer
                    PRIV_REQUIRES esp_adc
                    PRIV_REQUIRES nvs_flash
                    PRIV_REQUIRES esp_http_server
                    PRIV_REQUIRES json
                    )
