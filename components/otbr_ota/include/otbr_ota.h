/*
 * OTA updates for the ESP32-C5 OpenThread Border Router.
 *
 * A release is the app image plus web_storage, the SPIFFS image with the
 * border router's stock web UI (mounted at /spiffs by otbr_ota_start()).
 *
 * Runs its own small HTTP server (CONFIG_OTBR_OTA_HTTP_PORT, default 8080)
 * so that nothing in esp-thread-br has to be patched:
 *
 *   GET    /                        minimal update page
 *   GET    /api/ota/status          firmware, update state and mode (JSON)
 *   POST   /api/ota/check           check the release manifest now
 *   POST   /api/ota/install         install the release from the manifest
 *   POST   /api/ota/upload          install an app .bin (raw request body)
 *   POST   /api/ota/upload-storage  write a web_storage.bin (raw body)
 *   POST   /api/ota/mode            body "auto" or "manual"
 *   GET    /api/coredump            download the stored crash dump (ELF)
 *   DELETE /api/coredump            erase it
 *
 * POSTs and DELETEs must carry an "X-Thredge" header (any value). Browsers will not send
 * a custom header cross-origin without a CORS preflight this server never
 * approves, so another website cannot trigger an update.
 *
 * In auto mode the device checks the manifest periodically and installs
 * newer releases. In manual mode it only reports them.
 *
 * New images boot "pending verify" and are confirmed once they have been
 * online for CONFIG_OTBR_OTA_CONFIRM_DELAY_S; otherwise the bootloader rolls
 * back. A version that rolled back is never reinstalled automatically.
 * web_storage has no second slot: it is rewritten in place when its SHA-256
 * differs from the manifest's, and only together with (or after) the app
 * it belongs to.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start OTA support: mount web_storage at /spiffs, arm rollback
 *        confirmation, start the update worker and the update server (which
 *        comes up once the device has an IP address).
 *
 * Call once from app_main after NVS, esp_netif and the default event loop
 * are initialised, and before esp_br_web_start("/spiffs").
 */
esp_err_t otbr_ota_start(void);

#ifdef __cplusplus
}
#endif
