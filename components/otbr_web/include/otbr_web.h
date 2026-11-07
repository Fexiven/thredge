/*
 * Web UI support for the ESP32-C5 OpenThread Border Router.
 *
 * This component embeds the project's UI from web_ui/ into the application
 * and adds Wi-Fi REST endpoints:
 *
 *   GET  /api/wifi/status  current association, signal, band, IP, MAC
 *   POST /api/wifi/config  store new credentials and reconnect
 *   GET  /api/wifi/band    current band preference and associated band
 *   POST /api/wifi/band    store band preference and reconnect
 *
 * Thread data comes from the border router's own REST API (/node,
 * /get_properties, /node/dataset/active, ...), so none of that is duplicated
 * here. Routes are registered through esp_br_web_register_extra_uris(), which
 * the border router calls as its server starts; there is no start call.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
