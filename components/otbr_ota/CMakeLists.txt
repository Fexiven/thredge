# SPDX-License-Identifier: Apache-2.0
set(priv_requires esp_http_server
                  esp_https_ota
                  esp_http_client
                  app_update
                  esp_app_format
                  mbedtls
                  esp_event
                  esp_netif
                  esp_ot_br_server)

# IDF < 6.0 ships a built-in json component; from 6.0 it is gone and cJSON
# comes from the managed espressif/cjson dependency in idf_component.yml.
# Same pattern as third_party/esp-thread-br/components/esp_ot_br_server.
if("${IDF_VERSION_MAJOR}" VERSION_LESS "6")
    list(APPEND priv_requires json)
endif()

idf_component_register(SRCS "otbr_ota.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${priv_requires})
