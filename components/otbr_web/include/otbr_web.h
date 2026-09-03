/*
 * Web UI support for the ESP32-C5 OpenThread Border Router.
 *
 * This component owns the contents of the web_storage SPIFFS partition (the
 * project's own UI in web_ui/) and the Wi-Fi REST endpoints:
 *
 *   GET  /api/wifi/status  current association, signal, band, IP, MAC
 *   GET  /api/wifi/scan    nearby networks across both bands
 *   POST /api/wifi/config  store new credentials and reconnect
 *
 * Thread data comes from the border router's own REST API (/node,
 * /get_properties, /node/dataset/active, ...), so none of that is duplicated
 * here. Routes are registered through esp_br_web_register_extra_uris(), which
 * the border router web server calls as it starts; there is no start call.
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
 * @brief Register this project's routes on the border router web server.
 *
 * Overrides the weak default in esp_br_web.h; the border router calls it
 * while starting its server. Applications do not call this directly.
 *
 * @param[in] server Handle of the running web server
 */
void esp_br_web_register_extra_uris(httpd_handle_t server);

/**
 * @brief Initialise otbr_web early state.
 *
 * Registers a WIFI_EVENT_STA_START handler that applies a stored Wi-Fi band
 * preference (auto / 5 GHz only / 2.4 GHz only) as soon as Wi-Fi starts, so
 * it survives reboots. Call this from app_main after the default event loop
 * exists and before the border router launches Wi-Fi.
 */
esp_err_t otbr_web_init(void);

#ifdef __cplusplus
}
#endif
