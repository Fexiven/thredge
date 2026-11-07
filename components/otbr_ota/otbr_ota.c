/*
 * OTA updates for the ESP32-C5 OpenThread Border Router. See otbr_ota.h.
 *
 * All downloads and installs run on one worker task, so at most one update
 * is ever in flight. The upload handler runs on the HTTP server task but
 * claims the same busy flag before it touches flash.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otbr_ota.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "otbr_ota"

#define MANIFEST_MAX_SIZE 1024
#define URL_MAX_SIZE 512
#define VERSION_MAX_SIZE 32
#define ERROR_MAX_SIZE 96
#define UPLOAD_CHUNK_SIZE 4096
#define MAX_REDIRECTS 3

#define NVS_NAMESPACE "otbr_ota"
#define NVS_KEY_AUTO "auto"

/* Worker notification bits */
#define REQ_CHECK BIT0
#define REQ_INSTALL BIT1
#define REQ_RESTART BIT2

#define HOUR_MS (60ULL * 60ULL * 1000ULL)

/* The app description sits right after the image and first segment headers. */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define IMAGE_HEAD_SIZE (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

extern const char update_html_start[] asm("_binary_update_html_start");

typedef enum {
    STATE_IDLE,
    STATE_CHECKING,
    STATE_DOWNLOADING,
    STATE_UPLOADING,
    STATE_REBOOTING,
} ota_state_t;

static const char *const s_state_names[] = {"idle", "checking", "downloading", "uploading", "rebooting"};

/* Guards the strings and s_last_check_us below. */
static SemaphoreHandle_t s_lock;
static char s_latest_version[VERSION_MAX_SIZE];
static char s_latest_url[URL_MAX_SIZE];
static char s_last_error[ERROR_MAX_SIZE];
static int64_t s_last_check_us; /* 0 = never */

static atomic_bool s_busy;
static atomic_int s_state = STATE_IDLE;
static atomic_int s_progress = -1; /* percent, -1 = unknown */
static atomic_bool s_auto;
static atomic_bool s_online;

static TaskHandle_t s_worker;
static httpd_handle_t s_server;
static esp_timer_handle_t s_confirm_timer;
static esp_timer_handle_t s_deadline_timer;

/* ---------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ---------------------------------------------------------------------- */

static void set_error(const char *msg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_last_error, msg ? msg : "", sizeof(s_last_error));
    xSemaphoreGive(s_lock);
    if (msg && msg[0]) {
        ESP_LOGE(TAG, "%s", msg);
    }
}

static bool try_claim(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&s_busy, &expected, true);
}

static void release(void)
{
    atomic_store(&s_progress, -1);
    atomic_store(&s_state, STATE_IDLE);
    atomic_store(&s_busy, false);
}

static bool running_image_pending(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

/*
 * Compare dotted numeric versions ("2026.09.1", optional leading "v").
 * Returns >0 if a is newer, <0 if older, 0 if equal or either is not purely
 * numeric (a dev build, say), in which case "newer" cannot be decided.
 */
static int version_compare(const char *a, const char *b)
{
    if (*a == 'v') {
        a++;
    }
    if (*b == 'v') {
        b++;
    }
    while (*a || *b) {
        char *ea, *eb;
        unsigned long na = strtoul(a, &ea, 10);
        unsigned long nb = strtoul(b, &eb, 10);
        if ((*a && ea == a) || (*b && eb == b) || (*ea && *ea != '.') || (*eb && *eb != '.')) {
            return 0; /* not numeric */
        }
        if (na != nb) {
            return na > nb ? 1 : -1;
        }
        a = *ea ? ea + 1 : ea;
        b = *eb ? eb + 1 : eb;
    }
    return 0;
}

/* Version of the image the bootloader last rolled back from, if any. */
static bool last_invalid_version(char *out, size_t out_sz)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (bad == NULL || esp_ota_get_partition_description(bad, &desc) != ESP_OK) {
        return false;
    }
    strlcpy(out, desc.version, out_sz);
    return true;
}

/* An image for some other project would boot, fail and roll back at best. */
static bool image_matches_project(const esp_app_desc_t *desc, char *why, size_t why_sz)
{
    const esp_app_desc_t *running = esp_app_get_description();
    if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(why, why_sz, "Not an ESP-IDF application image");
        return false;
    }
    if (strncmp(desc->project_name, running->project_name, sizeof(desc->project_name)) != 0) {
        snprintf(why, why_sz, "Image is for \"%.32s\", not \"%.32s\"", desc->project_name, running->project_name);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Update mode                                                             */
/* ---------------------------------------------------------------------- */

static void mode_load(void)
{
#if CONFIG_OTBR_OTA_AUTO_DEFAULT
    bool value = true;
#else
    bool value = false;
#endif
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, NVS_KEY_AUTO, &v) == ESP_OK) {
            value = (v != 0);
        }
        nvs_close(h);
    }
    atomic_store(&s_auto, value);
}

static esp_err_t mode_save(bool automatic)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_AUTO, automatic ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        atomic_store(&s_auto, automatic);
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* Rollback confirmation                                                   */
/* ---------------------------------------------------------------------- */

static void confirm_timer_cb(void *arg)
{
    if (!running_image_pending()) {
        return;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "New firmware confirmed working, rollback cancelled");
        if (s_deadline_timer) {
            esp_timer_stop(s_deadline_timer);
        }
    } else {
        ESP_LOGE(TAG, "Failed to mark firmware as valid");
    }
}

static void deadline_timer_cb(void *arg)
{
    if (running_image_pending()) {
        ESP_LOGE(TAG, "New firmware never came online, rebooting to roll back");
        esp_restart();
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    atomic_store(&s_online, true);
    if (running_image_pending() && !esp_timer_is_active(s_confirm_timer)) {
        ESP_LOGI(TAG, "Pending firmware is online, confirming in %d s", CONFIG_OTBR_OTA_CONFIRM_DELAY_S);
        esp_timer_start_once(s_confirm_timer, (uint64_t)CONFIG_OTBR_OTA_CONFIRM_DELAY_S * 1000000ULL + 1);
    }
}

static void on_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    atomic_store(&s_online, false);
    /* Only an image that stays online for the whole delay gets confirmed. */
    esp_timer_stop(s_confirm_timer);
}

/* ---------------------------------------------------------------------- */
/* Manifest                                                                */
/* ---------------------------------------------------------------------- */

static esp_err_t fetch_manifest(char *version, size_t version_sz, char *url, size_t url_sz)
{
    char *buf = calloc(1, MANIFEST_MAX_SIZE + 1);
    if (buf == NULL) {
        set_error("Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = CONFIG_OTBR_OTA_MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(buf);
        set_error("Could not create the HTTP client");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    int status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            set_error("Could not connect to the manifest server");
            goto out;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            set_error("No response from the manifest server");
            goto out;
        }
        status = esp_http_client_get_status_code(client);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            break;
        }
        esp_http_client_flush_response(client, NULL);
        esp_http_client_close(client);
        if (esp_http_client_set_redirection(client) != ESP_OK) {
            break;
        }
    }
    if (status != 200) {
        char msg[ERROR_MAX_SIZE];
        snprintf(msg, sizeof(msg), "Manifest server answered HTTP %d", status);
        set_error(msg);
        goto out;
    }

    int total = 0;
    while (total < MANIFEST_MAX_SIZE) {
        int r = esp_http_client_read(client, buf + total, MANIFEST_MAX_SIZE - total);
        if (r < 0) {
            set_error("Manifest download failed");
            goto out;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    if (total >= MANIFEST_MAX_SIZE && !esp_http_client_is_complete_data_received(client)) {
        set_error("Manifest is too large");
        goto out;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    const cJSON *jver = root ? cJSON_GetObjectItem(root, "version") : NULL;
    const cJSON *jurl = root ? cJSON_GetObjectItem(root, "url") : NULL;
    if (!cJSON_IsString(jver) || !cJSON_IsString(jurl) || jver->valuestring[0] == '\0' ||
        strncmp(jurl->valuestring, "https://", 8) != 0) {
        set_error("Manifest is malformed");
    } else if (strlen(jver->valuestring) >= version_sz || strlen(jurl->valuestring) >= url_sz) {
        set_error("Manifest values are too long");
    } else {
        strlcpy(version, jver->valuestring, version_sz);
        strlcpy(url, jurl->valuestring, url_sz);
        ret = ESP_OK;
    }
    cJSON_Delete(root);

out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
    return ret;
}

/* Fetches the manifest and records the result. Worker task only. */
static esp_err_t do_check(void)
{
    char version[VERSION_MAX_SIZE] = "";
    char url[URL_MAX_SIZE] = "";

    atomic_store(&s_state, STATE_CHECKING);
    esp_err_t err = fetch_manifest(version, sizeof(version), url, sizeof(url));
    atomic_store(&s_state, STATE_IDLE);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_check_us = esp_timer_get_time();
    if (err == ESP_OK) {
        strlcpy(s_latest_version, version, sizeof(s_latest_version));
        strlcpy(s_latest_url, url, sizeof(s_latest_url));
        s_last_error[0] = '\0';
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Latest release: %s (running %s)", version, esp_app_get_description()->version);
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* Install from URL                                                        */
/* ---------------------------------------------------------------------- */

/* Downloads and installs url. On success the boot partition is switched and
 * the caller reboots. Worker task only, with the busy flag held. */
static esp_err_t do_install(const char *url)
{
    char why[ERROR_MAX_SIZE];
    ESP_LOGI(TAG, "Installing %s", url);
    atomic_store(&s_state, STATE_DOWNLOADING);
    atomic_store(&s_progress, 0);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 2048, /* release URLs redirect and get long */
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        snprintf(why, sizeof(why), "Download failed to start: %s", esp_err_to_name(err));
        set_error(why);
        return err;
    }

    esp_app_desc_t desc;
    err = esp_https_ota_get_img_desc(handle, &desc);
    if (err != ESP_OK) {
        snprintf(why, sizeof(why), "Could not read the image header: %s", esp_err_to_name(err));
        goto fail;
    }
    if (!image_matches_project(&desc, why, sizeof(why))) {
        err = ESP_ERR_INVALID_VERSION;
        goto fail;
    }

    const int size = esp_https_ota_get_image_size(handle);
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (size > 0) {
            atomic_store(&s_progress, (int)((int64_t)esp_https_ota_get_image_len_read(handle) * 100 / size));
        }
    }
    if (err != ESP_OK) {
        snprintf(why, sizeof(why), "Download failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        err = ESP_FAIL;
        snprintf(why, sizeof(why), "Download ended early");
        goto fail;
    }

    /* Validates the image (checksum, SHA-256) and switches the boot partition. */
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        snprintf(why, sizeof(why), "Image rejected: %s", esp_err_to_name(err));
        set_error(why);
        return err;
    }
    ESP_LOGI(TAG, "Installed %s", desc.version);
    return ESP_OK;

fail:
    esp_https_ota_abort(handle);
    set_error(why);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Worker                                                                  */
/* ---------------------------------------------------------------------- */

static void reboot_now(void)
{
    atomic_store(&s_state, STATE_REBOOTING);
    ESP_LOGI(TAG, "Rebooting into the new firmware");
    vTaskDelay(pdMS_TO_TICKS(2000)); /* lets the HTTP reply and logs go out */
    esp_restart();
}

/* Returns true if the latest release should be installed. Sets an error
 * explaining why not, for the cases the user asked for. */
static bool should_install(bool manual)
{
    char latest[VERSION_MAX_SIZE], url[URL_MAX_SIZE], bad[VERSION_MAX_SIZE];
    const char *running = esp_app_get_description()->version;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(latest, s_latest_version, sizeof(latest));
    strlcpy(url, s_latest_url, sizeof(url));
    xSemaphoreGive(s_lock);

    if (latest[0] == '\0' || url[0] == '\0') {
        return false;
    }
    if (running_image_pending()) {
        /* Installing now would overwrite the slot we would roll back to. */
        if (manual) {
            set_error("The running firmware is not confirmed yet; try again in a minute");
        }
        return false;
    }
    if (manual) {
        /* A person asked: install whatever the manifest names, unless it is
         * what is already running. Allows going back to a release. */
        if (strcmp(latest, running) == 0) {
            set_error("Already running the latest release");
            return false;
        }
        return true;
    }
    if (version_compare(latest, running) <= 0) {
        return false;
    }
    if (last_invalid_version(bad, sizeof(bad)) && strcmp(bad, latest) == 0) {
        ESP_LOGW(TAG, "Not installing %s automatically: it rolled back before", latest);
        return false;
    }
    return true;
}

static void install_latest(void)
{
    char url[URL_MAX_SIZE];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(url, s_latest_url, sizeof(url));
    xSemaphoreGive(s_lock);

    if (do_install(url) == ESP_OK) {
        reboot_now();
    }
}

static void worker_task(void *arg)
{
    TickType_t next_auto = xTaskGetTickCount() + pdMS_TO_TICKS(CONFIG_OTBR_OTA_FIRST_CHECK_DELAY_S * 1000ULL);

    while (true) {
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (int32_t)(next_auto - now) > 0 ? next_auto - now : 0;
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);

        if (bits & REQ_RESTART) {
            reboot_now();
        }

        bool manual = (bits & (REQ_CHECK | REQ_INSTALL)) != 0;
        bool timer_due = (int32_t)(xTaskGetTickCount() - next_auto) >= 0;
        if (!manual && !timer_due) {
            continue;
        }
        if (!atomic_load(&s_online)) {
            if (manual) {
                set_error("No network connection");
            }
            if (timer_due) {
                next_auto = xTaskGetTickCount() + pdMS_TO_TICKS(60 * 1000); /* re-evaluate shortly */
            }
            continue;
        }
        if (!try_claim()) {
            continue; /* an upload is running */
        }

        esp_err_t err = do_check();
        if (timer_due || manual) {
            next_auto = xTaskGetTickCount() +
                        pdMS_TO_TICKS((err == ESP_OK ? CONFIG_OTBR_OTA_CHECK_INTERVAL_HOURS : 1) * HOUR_MS);
        }
        if (err == ESP_OK) {
            if (bits & REQ_INSTALL) {
                if (should_install(true)) {
                    install_latest();
                }
            } else if (!manual && atomic_load(&s_auto) && should_install(false)) {
                install_latest();
            }
        }
        release();
    }
}

static esp_err_t notify_worker(uint32_t bits)
{
    if (s_worker == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotify(s_worker, bits, eSetBits);
    return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/* HTTP                                                                    */
/* ---------------------------------------------------------------------- */

static esp_err_t send_json_status(httpd_req_t *req, const char *status, const char *key, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    char *out = NULL;
    if (root && cJSON_AddStringToObject(root, key, msg)) {
        out = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    cJSON_free(out);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    return send_json_status(req, status, "error", msg);
}

/* Rejects POSTs a foreign web page could have made: see otbr_ota.h. */
static bool guard_post(httpd_req_t *req)
{
    if (httpd_req_get_hdr_value_len(req, "X-Thredge") > 0) {
        return true;
    }
    send_error(req, "403 Forbidden", "Missing X-Thredge header");
    return false;
}

static esp_err_t page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, update_html_start);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char latest[VERSION_MAX_SIZE], error[ERROR_MAX_SIZE], bad[VERSION_MAX_SIZE];
    int64_t last_check;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(latest, s_latest_version, sizeof(latest));
    strlcpy(error, s_last_error, sizeof(error));
    last_check = s_last_check_us;
    xSemaphoreGive(s_lock);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    cJSON_AddStringToObject(root, "project", app->project_name);
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "idf", app->idf_ver);
    cJSON_AddStringToObject(root, "partition", running ? running->label : "");
    cJSON_AddBoolToObject(root, "pending_verify", running_image_pending());
    cJSON_AddStringToObject(root, "mode", atomic_load(&s_auto) ? "auto" : "manual");
    cJSON_AddStringToObject(root, "state", s_state_names[atomic_load(&s_state)]);
    int progress = atomic_load(&s_progress);
    if (progress >= 0) {
        cJSON_AddNumberToObject(root, "progress", progress);
    }
    if (latest[0]) {
        cJSON_AddStringToObject(root, "latest", latest);
        /* Undecidable comparisons (a dev build) count as "different". */
        int cmp = version_compare(latest, app->version);
        cJSON_AddBoolToObject(root, "update_available", cmp > 0 || (cmp == 0 && strcmp(latest, app->version) != 0));
    }
    if (last_check) {
        cJSON_AddNumberToObject(root, "last_check_s_ago", (double)((esp_timer_get_time() - last_check) / 1000000));
    }
    if (last_invalid_version(bad, sizeof(bad))) {
        cJSON_AddStringToObject(root, "rolled_back_from", bad);
    }
    if (error[0]) {
        cJSON_AddStringToObject(root, "error", error);
    }
    cJSON_AddStringToObject(root, "manifest_url", CONFIG_OTBR_OTA_MANIFEST_URL);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

static esp_err_t check_post_handler(httpd_req_t *req)
{
    if (!guard_post(req)) {
        return ESP_OK;
    }
    if (atomic_load(&s_busy)) {
        return send_error(req, "409 Conflict", "An update is already in progress");
    }
    set_error(NULL);
    notify_worker(REQ_CHECK);
    return send_json_status(req, "202 Accepted", "status", "Checking for updates");
}

static esp_err_t install_post_handler(httpd_req_t *req)
{
    if (!guard_post(req)) {
        return ESP_OK;
    }
    if (atomic_load(&s_busy)) {
        return send_error(req, "409 Conflict", "An update is already in progress");
    }
    set_error(NULL);
    notify_worker(REQ_INSTALL);
    return send_json_status(req, "202 Accepted", "status",
                            "Installing the latest release. The device reboots when it is done.");
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
    if (!guard_post(req)) {
        return ESP_OK;
    }
    char body[16] = "";
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return send_error(req, "400 Bad Request", "Body must be \"auto\" or \"manual\"");
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';
    /* Tolerate a trailing newline from curl -d @file or echo. */
    body[strcspn(body, "\r\n ")] = '\0';

    bool automatic;
    if (strcmp(body, "auto") == 0) {
        automatic = true;
    } else if (strcmp(body, "manual") == 0) {
        automatic = false;
    } else {
        return send_error(req, "400 Bad Request", "Body must be \"auto\" or \"manual\"");
    }
    if (mode_save(automatic) != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "Could not save the mode");
    }
    ESP_LOGI(TAG, "Update mode set to %s", body);
    return send_json_status(req, NULL, "mode", body);
}

/* Receives exactly len bytes unless the client goes away. */
static int recv_full(httpd_req_t *req, char *buf, int len)
{
    int got = 0;
    int timeouts = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return -1;
            }
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        timeouts = 0;
        got += r;
    }
    return got;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    if (!guard_post(req)) {
        return ESP_OK;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    int remaining = req->content_len;
    if (remaining < (int)IMAGE_HEAD_SIZE) {
        return send_error(req, "400 Bad Request", "Upload is empty or too small to be a firmware image");
    }
    if (target == NULL) {
        return send_error(req, "500 Internal Server Error", "No OTA partition found");
    }
    if ((size_t)remaining > target->size) {
        return send_error(req, "413 Payload Too Large", "Image is larger than the OTA partition");
    }
    if (running_image_pending()) {
        return send_error(req, "409 Conflict", "The running firmware is not confirmed yet; try again in a minute");
    }
    if (!try_claim()) {
        return send_error(req, "409 Conflict", "An update is already in progress");
    }

    char *buf = malloc(UPLOAD_CHUNK_SIZE);
    if (buf == NULL) {
        release();
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    atomic_store(&s_state, STATE_UPLOADING);
    atomic_store(&s_progress, 0);
    set_error(NULL);
    ESP_LOGI(TAG, "Receiving upload (%d bytes) into %s", remaining, target->label);

    const int total = remaining;
    char why[ERROR_MAX_SIZE] = "";
    esp_ota_handle_t ota = 0;
    esp_err_t err = ESP_OK;

    /* Check the header before erasing anything. */
    int first = remaining < UPLOAD_CHUNK_SIZE ? remaining : UPLOAD_CHUNK_SIZE;
    if (recv_full(req, buf, first) != first) {
        err = ESP_FAIL;
        snprintf(why, sizeof(why), "Upload interrupted");
    } else if ((uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
        err = ESP_ERR_INVALID_ARG;
        snprintf(why, sizeof(why), "Not a firmware image (use esp32c5_otbr.bin, not the merged file)");
    } else {
        esp_app_desc_t desc;
        memcpy(&desc, buf + APP_DESC_OFFSET, sizeof(desc));
        if (!image_matches_project(&desc, why, sizeof(why))) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            ESP_LOGI(TAG, "Upload is %s %s", desc.project_name, desc.version);
            err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (err != ESP_OK) {
                snprintf(why, sizeof(why), "Could not prepare flash: %s", esp_err_to_name(err));
            }
        }
    }

    int len = first;
    while (err == ESP_OK) {
        err = esp_ota_write(ota, buf, len);
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "Flash write failed: %s", esp_err_to_name(err));
            break;
        }
        remaining -= len;
        atomic_store(&s_progress, (int)((int64_t)(total - remaining) * 100 / total));
        if (remaining == 0) {
            break;
        }
        len = remaining < UPLOAD_CHUNK_SIZE ? remaining : UPLOAD_CHUNK_SIZE;
        if (recv_full(req, buf, len) != len) {
            err = ESP_FAIL;
            snprintf(why, sizeof(why), "Upload interrupted");
        }
    }
    free(buf);

    if (err == ESP_OK) {
        err = esp_ota_end(ota); /* validates checksum and SHA-256 */
        ota = 0;
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "Image rejected: %s", esp_err_to_name(err));
        }
    }
    if (ota != 0) {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(target);
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "Could not switch boot partition: %s", esp_err_to_name(err));
        }
    }

    if (err != ESP_OK) {
        set_error(why);
        release();
        return send_error(req, "400 Bad Request", why);
    }

    ESP_LOGI(TAG, "Upload installed");
    atomic_store(&s_state, STATE_REBOOTING);
    esp_err_t sent = send_json_status(req, NULL, "status", "Installed. Rebooting into the new firmware.");
    notify_worker(REQ_RESTART); /* the busy flag stays held until the reboot */
    return sent;
}

static void start_server(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_OTBR_OTA_HTTP_PORT;
    /* The border router's server already uses the default control port. */
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    config.max_open_sockets = 3;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.stack_size = 6144;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the update server");
        s_server = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get_handler},
        {.uri = "/api/ota/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/ota/check", .method = HTTP_POST, .handler = check_post_handler},
        {.uri = "/api/ota/install", .method = HTTP_POST, .handler = install_post_handler},
        {.uri = "/api/ota/upload", .method = HTTP_POST, .handler = upload_post_handler},
        {.uri = "/api/ota/mode", .method = HTTP_POST, .handler = mode_post_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    ESP_LOGI(TAG, "Update server on port %d", CONFIG_OTBR_OTA_HTTP_PORT);
}

static void on_got_ip_start_server(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* Started lazily: in SoftAP setup mode there is nothing to update yet. */
    start_server();
}

/* ---------------------------------------------------------------------- */
/* Startup                                                                 */
/* ---------------------------------------------------------------------- */

esp_err_t otbr_ota_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    mode_load();

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware %s %s, update mode %s", app->project_name, app->version,
             atomic_load(&s_auto) ? "auto" : "manual");

    const esp_timer_create_args_t confirm_args = {.callback = confirm_timer_cb, .name = "ota_confirm"};
    const esp_timer_create_args_t deadline_args = {.callback = deadline_timer_cb, .name = "ota_deadline"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&confirm_args, &s_confirm_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_create(&deadline_args, &s_deadline_timer), TAG, "timer");

    if (running_image_pending()) {
        ESP_LOGW(TAG, "Running firmware is pending verification");
#if CONFIG_OTBR_OTA_CONFIRM_DEADLINE_MIN > 0
        esp_timer_start_once(s_deadline_timer, (uint64_t)CONFIG_OTBR_OTA_CONFIRM_DEADLINE_MIN * 60ULL * 1000000ULL);
#endif
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL, NULL),
                        TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_lost_ip, NULL, NULL),
                        TAG, "event");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip_start_server,
                                                            NULL, NULL),
                        TAG, "event");

    /* Large stack: TLS handshake plus esp_https_ota. */
    if (xTaskCreate(worker_task, "otbr_ota", 8192, NULL, 4, &s_worker) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
