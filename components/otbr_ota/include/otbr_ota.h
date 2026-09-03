/*
 * OTA update support for the ESP32-C5 OpenThread Border Router.
 *
 * Provides:
 *  - A firmware update page on the border router's own web server (port 80),
 *    registered through esp_br_web_register_extra_uris():
 *      GET  /ota.html        update page, linked from the "Update" nav tab
 *      GET  /api/ota/version running firmware info
 *      GET  /api/ota/check   compare against the release manifest
 *      POST /api/ota/update  upload a .bin directly (raw body)
 *      POST /api/ota/install download + install the manifest release
 *  - Automatic rollback confirmation: when a freshly installed image gets
 *    an IP address, it is marked valid; if it never does, the bootloader
 *    rolls back to the previous image on the next reset.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Arm the rollback confirmation handler.
 *
 * Call once at startup, after the default event loop exists. The update page
 * itself needs no start call: it is registered on the border router web
 * server when that server comes up.
 */
esp_err_t otbr_ota_start(void);

/**
 * @brief Register the OTA REST endpoints on an existing web server.
 *
 * Called by otbr_web when the border router web server comes up. The update
 * page itself is an ordinary file in the web UI, so only the API is here.
 *
 * @param[in] server Handle of the running web server
 */
void otbr_ota_register_uris(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
