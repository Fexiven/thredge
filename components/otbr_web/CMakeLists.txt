# SPDX-License-Identifier: Apache-2.0
# openthread is listed because esp_ot_wifi_cmd.h includes <openthread/error.h>
# and esp_ot_cli_extension keeps openthread in its PRIV_REQUIRES, so the
# include path does not reach consumers. Requiring it here avoids patching
# upstream for it.
set(priv_requires esp_ot_br_server
                  esp_ot_cli_extension
                  otbr_ota
                  openthread
                  esp_wifi
                  esp_netif
                  esp_http_server
                  nvs_flash)

# IDF < 6.0 ships a built-in json component; from 6.0 it is gone and cJSON
# comes from the managed espressif/cjson dependency in idf_component.yml.
if("${IDF_VERSION_MAJOR}" VERSION_LESS "6")
    list(APPEND priv_requires json)
endif()

# WHOLE_ARCHIVE is required, not cosmetic. Nothing calls into this component:
# it works by overriding the weak esp_br_web_register_extra_uris() in
# esp_ot_br_server. Without it the linker never pulls this object out of the
# archive, the weak no-op stays, and every route below silently 404s.
# The UI is embedded in the binary rather than flashed to a SPIFFS partition,
# so an OTA update carries the pages with it and rollback covers them too.
# Uncompressed: the whole UI is ~42 kB against a megabyte of free app slot,
# which is not worth a gzip step in the build.
idf_component_register(SRCS "otbr_web.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${priv_requires}
                       EMBED_FILES "web_ui/index.html"
                                   "web_ui/wifi.html"
                                   "web_ui/thread.html"
                                   "web_ui/map.html"
                                   "web_ui/update.html"
                                   "web_ui/static/style.css"
                                   "web_ui/static/app.js"
                       WHOLE_ARCHIVE)
