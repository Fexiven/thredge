/*
 * ESP32-C5 single-chip OpenThread Border Router
 *
 * Boot flow (all handled by the thread_border_router component once
 * launched):
 *   1. Try Wi-Fi credentials stored in NVS.
 *   2. None stored / connection fails -> SoftAP "ESP-ThreadBR-XXXX" with a
 *      captive portal at http://192.168.4.1 for Wi-Fi setup.
 *   3. Connected -> border router starts, forms (or rejoins) a Thread
 *      network, Web GUI on port 80 with the firmware update page on its
 *      "Update" tab.
 *
 * Holding the BOOT button (GPIO28) for 5 s erases the stored Wi-Fi
 * credentials and reboots into the setup AP.
 *
 * Adapted from esp-thread-br/examples/basic_thread_border_router.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "border_router_launch.h"
#if CONFIG_OPENTHREAD_BR_START_WEB
#include "esp_br_web.h"
#endif
#include "otbr_ota.h"
#include "otbr_web.h"

#define TAG "otbr_main"

/* Namespace used by esp_ot_wifi_cmd (esp_ot_cli_extension) for stored
 * Wi-Fi credentials. Erased directly so the factory reset works no matter
 * what state the Wi-Fi stack is in. */
#define WIFI_CONFIG_NVS_NAMESPACE "wifi_config"

static void factory_reset_wifi(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGW(TAG, "Wi-Fi credentials erased");
    } else {
        ESP_LOGW(TAG, "No stored Wi-Fi credentials to erase (0x%x)", err);
    }
    ESP_LOGW(TAG, "Rebooting into setup mode ...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void factory_reset_button_task(void *arg)
{
    const gpio_num_t pin = (gpio_num_t)CONFIG_OTBR_FACTORY_RESET_GPIO;
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    int held_ms = 0;
    bool warned = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(pin) == 0) { /* button pressed (active low) */
            held_ms += 100;
            if (!warned && held_ms >= 1000) {
                ESP_LOGW(TAG, "Keep holding BOOT to factory-reset Wi-Fi ...");
                warned = true;
            }
            if (held_ms >= CONFIG_OTBR_FACTORY_RESET_HOLD_MS) {
                factory_reset_wifi();
            }
        } else {
            held_ms = 0;
            warned = false;
        }
    }
}

void app_main(void)
{
    /* Used eventfds: netif, task queue, border router (+1 spare/TREL). */
    size_t max_eventfd = 4;
#if CONFIG_OPENTHREAD_RADIO_TREL
    max_eventfd++;
#endif
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = max_eventfd,
    };

    esp_openthread_config_t openthread_config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config =
            {
                .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
                .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
                .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
            },
    };
    esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();

    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_OTBR_MDNS_HOSTNAME));

    /* Arms rollback confirmation. The update page and its API attach to the
     * border router's own web server via esp_br_web_register_extra_uris(). */
    ESP_ERROR_CHECK(otbr_ota_start());

    /* Registers a WIFI_EVENT_STA_START handler that applies the stored band
     * preference before the border router launches Wi-Fi below. */
    ESP_ERROR_CHECK(otbr_web_init());

    xTaskCreate(factory_reset_button_task, "factory_reset", 3072, NULL, 3, NULL);

#if CONFIG_OPENTHREAD_BR_START_WEB
    /* Base path is unused: pages are served from the binary by otbr_web. */
    esp_br_web_start("");
#endif

    launch_openthread_border_router(&openthread_config, &rcp_update_config);
}
