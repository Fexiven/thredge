# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES otbr_ota otbr_web esp_http_server)
