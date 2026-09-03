/*
 * OTA update support for the ESP32-C5 OpenThread Border Router.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otbr_ota.h"

#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_br_web.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "otbr_ota"

#define UPLOAD_CHUNK_SIZE 4096
#define MANIFEST_MAX_SIZE 1024
#define URL_MAX_SIZE 512
#define VERSION_MAX_SIZE 32


static bool s_update_in_progress = false;
static char s_manifest_version[VERSION_MAX_SIZE] = "";
static char s_manifest_url[URL_MAX_SIZE] = "";

/* ---------------------------------------------------------------------- */
/* Rollback confirmation                                                   */
/* ---------------------------------------------------------------------- */

static void mark_app_valid_once(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "New firmware confirmed working, rollback cancelled");
        } else {
            ESP_LOGE(TAG, "Failed to mark firmware as valid");
        }
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* Network is up: this build has proven itself. */
    mark_app_valid_once();
}

/* ---------------------------------------------------------------------- */
/* Manifest fetch (GitHub Pages / any HTTPS host in the cert bundle)       */
/* ---------------------------------------------------------------------- */

static esp_err_t fetch_manifest(char *version, size_t version_sz, char *url, size_t url_sz)
{
    char *buf = calloc(1, MANIFEST_MAX_SIZE + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_FAIL;
    esp_http_client_config_t config = {
        .url = CONFIG_OTBR_OTA_MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(buf);
        return ESP_FAIL;
    }

    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open connection to manifest URL");
            break;
        }
        (void)esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "Manifest fetch returned HTTP %d", status);
            break;
        }
        int total = 0;
        while (total < MANIFEST_MAX_SIZE) {
            int r = esp_http_client_read(client, buf + total, MANIFEST_MAX_SIZE - total);
            if (r <= 0) {
                break;
            }
            total += r;
        }
        buf[total] = '\0';

        cJSON *root = cJSON_Parse(buf);
        if (root == NULL) {
            ESP_LOGE(TAG, "Manifest is not valid JSON");
            break;
        }
        const cJSON *jver = cJSON_GetObjectItem(root, "version");
        const cJSON *jurl = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(jver) && cJSON_IsString(jurl)) {
            strlcpy(version, jver->valuestring, version_sz);
            strlcpy(url, jurl->valuestring, url_sz);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Manifest is missing \"version\" or \"url\"");
        }
        cJSON_Delete(root);
    } while (0);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
    return ret;
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void schedule_restart(void)
{
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------------------------------------------- */
/* HTTP handlers                                                           */
/* ---------------------------------------------------------------------- */


static esp_err_t version_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char json[192];
    snprintf(json, sizeof(json), "{\"project\":\"%s\",\"version\":\"%s\",\"idf\":\"%s\"}", app->project_name,
             app->version, app->idf_ver);
    return send_json(req, json);
}

static esp_err_t check_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char version[VERSION_MAX_SIZE];
    char url[URL_MAX_SIZE];

    if (fetch_manifest(version, sizeof(version), url, sizeof(url)) != ESP_OK) {
        httpd_resp_set_status(req, "502 Bad Gateway");
        return send_json(req, "{\"error\":\"Could not fetch the release manifest\"}");
    }

    strlcpy(s_manifest_version, version, sizeof(s_manifest_version));
    strlcpy(s_manifest_url, url, sizeof(s_manifest_url));

    bool update_available = (strcmp(version, app->version) != 0);
    char json[256];
    snprintf(json, sizeof(json), "{\"current\":\"%s\",\"latest\":\"%s\",\"update_available\":%s}", app->version,
             version, update_available ? "true" : "false");
    return send_json(req, json);
}

static void https_ota_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(TAG, "Starting HTTPS OTA from %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size_tx = 2048, /* release URLs redirect and get long */
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t err = esp_https_ota(&ota_config);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA install complete, rebooting into new firmware");
        schedule_restart();
    } else {
        ESP_LOGE(TAG, "HTTPS OTA failed: %s", esp_err_to_name(err));
        s_update_in_progress = false;
    }
    vTaskDelete(NULL);
}

static esp_err_t install_post_handler(httpd_req_t *req)
{
    if (s_update_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"error\":\"An update is already in progress\"}");
    }
    if (s_manifest_url[0] == '\0') {
        /* No prior /api/check in this boot: fetch the manifest now. */
        if (fetch_manifest(s_manifest_version, sizeof(s_manifest_version), s_manifest_url, sizeof(s_manifest_url)) !=
            ESP_OK) {
            httpd_resp_set_status(req, "502 Bad Gateway");
            return send_json(req, "{\"error\":\"Could not fetch the release manifest\"}");
        }
    }

    char *url = strdup(s_manifest_url);
    if (url == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Out of memory\"}");
    }

    s_update_in_progress = true;
    if (xTaskCreate(https_ota_task, "https_ota", 8192, url, 5, NULL) != pdPASS) {
        free(url);
        s_update_in_progress = false;
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Failed to start the update task\"}");
    }
    return send_json(req, "{\"status\":\"Download started. The device reboots when the install finishes.\"}");
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (s_update_in_progress) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, "{\"error\":\"An update is already in progress\"}");
    }

    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"Empty upload\"}");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"No OTA partition found\"}");
    }
    if ((size_t)remaining > update_partition->size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return send_json(req, "{\"error\":\"Image is larger than the OTA partition\"}");
    }

    char *buf = malloc(UPLOAD_CHUNK_SIZE);
    if (buf == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Out of memory\"}");
    }

    s_update_in_progress = true;
    ESP_LOGI(TAG, "Receiving firmware upload (%d bytes) into partition %s", remaining, update_partition->label);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    while (err == ESP_OK && remaining > 0) {
        int received = httpd_req_recv(req, buf, remaining < UPLOAD_CHUNK_SIZE ? remaining : UPLOAD_CHUNK_SIZE);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota_handle, buf, received);
        remaining -= received;
    }
    free(buf);

    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
    } else if (ota_handle != 0) {
        esp_ota_abort(ota_handle);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(update_partition);
    }

    if (err != ESP_OK) {
        s_update_in_progress = false;
        ESP_LOGE(TAG, "Firmware upload failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"Upload failed or the image is invalid\"}");
    }

    ESP_LOGI(TAG, "Upload complete, rebooting into new firmware");
    esp_err_t resp = send_json(req, "{\"status\":\"Update installed. Rebooting ...\"}");
    schedule_restart();
    return resp;
}

/* ---------------------------------------------------------------------- */
/* Startup                                                                 */
/* ---------------------------------------------------------------------- */

/* Registered by otbr_web alongside its own routes. The update page itself is
 * an ordinary file in the web UI, so only the API lives here. */
void otbr_ota_register_uris(httpd_handle_t server)
{
    const httpd_uri_t version = {.uri = "/api/ota/version", .method = HTTP_GET, .handler = version_get_handler};
    const httpd_uri_t check = {.uri = "/api/ota/check", .method = HTTP_GET, .handler = check_get_handler};
    const httpd_uri_t install = {.uri = "/api/ota/install", .method = HTTP_POST, .handler = install_post_handler};
    const httpd_uri_t update = {.uri = "/api/ota/update", .method = HTTP_POST, .handler = update_post_handler};
    httpd_register_uri_handler(server, &version);
    httpd_register_uri_handler(server, &check);
    httpd_register_uri_handler(server, &install);
    httpd_register_uri_handler(server, &update);

    ESP_LOGI(TAG, "OTA API registered under /api/ota/");
}

esp_err_t otbr_ota_start(void)
{
    /* The update UI rides on the border router's own web server, so all this
     * has to do is arm the rollback confirmation. */
    return esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL);
}
