/*
 * OTA updates for the ESP32-C5 OpenThread Border Router.
 *
 * Runs its own small HTTP server (CONFIG_OTBR_OTA_HTTP_PORT, default 8080)
 * so that nothing in esp-thread-br has to be patched:
 *
 *   GET  /                 minimal update page
 *   GET  /api/ota/status   firmware, update state and mode (JSON)
 *   POST /api/ota/check    check the release manifest now
 *   POST /api/ota/install  install the release from the manifest
 *   POST /api/ota/upload   install an uploaded .bin (raw request body)
 *   POST /api/ota/mode     body "auto" or "manual"
 *
 * POSTs must carry an "X-Thredge" header (any value). Browsers will not send
 * a custom header cross-origin without a CORS preflight this server never
 * approves, so another website cannot trigger an update.
 *
 * In auto mode the device checks the manifest periodically and installs
 * newer releases. In manual mode it only reports them.
 *
 * New images boot "pending verify" and are confirmed once they have been
 * online for CONFIG_OTBR_OTA_CONFIRM_DELAY_S; otherwise the bootloader rolls
 * back. A version that rolled back is never reinstalled automatically.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start OTA support: rollback confirmation, the update worker and the
 *        update server (which comes up once the device has an IP address).
 *
 * Call once from app_main after NVS, esp_netif and the default event loop
 * are initialised.
 */
esp_err_t otbr_ota_start(void);

#ifdef __cplusplus
}
#endif
