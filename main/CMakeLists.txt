idf_component_register(SRCS "main.c" "webdav.c" "cacert_server.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_wifi esp_event esp_netif nvs_flash
                                 esp_http_server esp_https_server mbedtls
                                 fatfs sdmmc driver esp_system
                        EMBED_TXTFILES "certs/server.crt" "certs/server.key"
                                       "certs/ca.crt")
