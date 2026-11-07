/*
 * OTA updates for the ESP32-C5 OpenThread Border Router. See otbr_ota.h.
 *
 * A release is two images:
 *   - the app, installed into the inactive A/B slot with bootloader rollback
 *   - web_storage, the SPIFFS image holding the border router's stock web UI,
 *     rewritten in place whenever its SHA-256 differs from the manifest's
 *
 * All downloads and installs run on one worker task, so at most one update
 * is ever in flight. The upload handlers run on the HTTP server task but
 * claim the same busy flag before they touch flash.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otbr_ota.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_core_dump.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "otbr_ota"

#define MANIFEST_MAX_SIZE 2048
#define URL_MAX_SIZE 512
#define VERSION_MAX_SIZE 32
#define SHA_HEX_SIZE 65
#define ERROR_MAX_SIZE 96
#define CHUNK_SIZE 4096
#define MAX_REDIRECTS 3

#define NVS_NAMESPACE "otbr_ota"
#define NVS_KEY_AUTO "auto"

#define STORAGE_LABEL "web_storage"
#define STORAGE_BASE_PATH "/spiffs" /* upstream's SoftAP page looks here too */

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
    STATE_WRITING_STORAGE,
    STATE_UPLOADING,
    STATE_REBOOTING,
} ota_state_t;

static const char *const s_state_names[] = {"idle",      "checking", "downloading", "writing web storage",
                                            "uploading", "rebooting"};

typedef struct {
    char version[VERSION_MAX_SIZE];
    char url[URL_MAX_SIZE];
    char storage_url[URL_MAX_SIZE]; /* "" if the release has none */
    char storage_sha[SHA_HEX_SIZE];
} manifest_t;

/* Guards everything in this block. */
static SemaphoreHandle_t s_lock;
static manifest_t s_manifest; /* version[0] == '\0' until a check succeeds */
static char s_storage_sha[SHA_HEX_SIZE];
static char s_last_error[ERROR_MAX_SIZE];
static int64_t s_last_check_us; /* 0 = never */

static atomic_bool s_busy;
static atomic_int s_state = STATE_IDLE;
static atomic_int s_progress = -1; /* percent, -1 = unknown */
static atomic_bool s_auto;
static atomic_bool s_online;
static bool s_storage_mounted; /* worker / busy holder only */

static const esp_partition_t *s_storage;
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

static void get_manifest(manifest_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_manifest;
    xSemaphoreGive(s_lock);
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

static bool is_sha_hex(const char *s)
{
    if (strlen(s) != SHA_HEX_SIZE - 1) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Web storage (SPIFFS)                                                    */
/* ---------------------------------------------------------------------- */

static esp_err_t storage_mount(void)
{
    if (s_storage_mounted) {
        return ESP_OK;
    }
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = STORAGE_LABEL,
        .max_files = 10,
        .format_if_mount_failed = false, /* a blank UI is worse than a visible error */
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        s_storage_mounted = true;
    } else {
        ESP_LOGE(TAG, "Could not mount %s: %s", STORAGE_LABEL, esp_err_to_name(err));
    }
    return err;
}

static void storage_unmount(void)
{
    if (s_storage_mounted) {
        esp_vfs_spiffs_unregister(STORAGE_LABEL);
        s_storage_mounted = false;
    }
}

/* Hashes the whole partition, as CI hashes the whole (padded) image. */
static void storage_hash_update(void)
{
    uint8_t sha[32];
    char hex[SHA_HEX_SIZE] = "";
    if (s_storage && esp_partition_get_sha256(s_storage, sha) == ESP_OK) {
        for (int i = 0; i < 32; i++) {
            snprintf(hex + i * 2, 3, "%02x", sha[i]);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_storage_sha, hex, sizeof(s_storage_sha));
    xSemaphoreGive(s_lock);
}

/* True if the manifest has a web_storage image that differs from ours. */
static bool storage_differs(const manifest_t *m)
{
    if (s_storage == NULL || m->storage_url[0] == '\0') {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool differs = strcasecmp(m->storage_sha, s_storage_sha) != 0;
    xSemaphoreGive(s_lock);
    return differs;
}

/* Pulls up to len bytes from a source; returns bytes read, 0 at end, <0 on error. */
typedef int (*read_fn_t)(void *ctx, char *buf, int len);

/*
 * Replaces web_storage with total bytes from read. The partition is
 * unmounted for the duration and remounted afterwards. expect_sha (may be
 * NULL) is checked against the written partition.
 */
static esp_err_t storage_write(read_fn_t read, void *ctx, int total, const char *expect_sha, char *why,
                               size_t why_sz)
{
    if (s_storage == NULL) {
        snprintf(why, why_sz, "No web_storage partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (total != (int)s_storage->size) {
        snprintf(why, why_sz, "Web storage image is %d bytes, partition is %" PRIu32, total, s_storage->size);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc(CHUNK_SIZE);
    if (buf == NULL) {
        snprintf(why, why_sz, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_state, STATE_WRITING_STORAGE);
    atomic_store(&s_progress, 0);
    ESP_LOGI(TAG, "Rewriting %s (%d bytes)", STORAGE_LABEL, total);
    storage_unmount();

    esp_err_t err = esp_partition_erase_range(s_storage, 0, s_storage->size);
    if (err != ESP_OK) {
        snprintf(why, why_sz, "Erase failed: %s", esp_err_to_name(err));
    }
    int done = 0;
    while (err == ESP_OK && done < total) {
        int want = total - done < CHUNK_SIZE ? total - done : CHUNK_SIZE;
        int got = 0;
        while (got < want) {
            int r = read(ctx, buf + got, want - got);
            if (r <= 0) {
                break;
            }
            got += r;
        }
        if (got != want) {
            err = ESP_FAIL;
            snprintf(why, why_sz, "Web storage download interrupted");
            break;
        }
        err = esp_partition_write(s_storage, done, buf, want);
        if (err != ESP_OK) {
            snprintf(why, why_sz, "Flash write failed: %s", esp_err_to_name(err));
            break;
        }
        done += want;
        atomic_store(&s_progress, (int)((int64_t)done * 100 / total));
    }
    free(buf);

    storage_hash_update();
    if (err == ESP_OK && expect_sha) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool match = strcasecmp(expect_sha, s_storage_sha) == 0;
        xSemaphoreGive(s_lock);
        if (!match) {
            err = ESP_ERR_INVALID_CRC;
            snprintf(why, why_sz, "Web storage checksum mismatch after writing");
        }
    }
    if (storage_mount() != ESP_OK && err == ESP_OK) {
        err = ESP_FAIL;
        snprintf(why, why_sz, "Written web storage does not mount");
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Web storage updated");
    }
    return err;
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
        esp_timer_stop(s_deadline_timer);
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

static void start_server(void);

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    atomic_store(&s_online, true);
    if (running_image_pending() && !esp_timer_is_active(s_confirm_timer)) {
        ESP_LOGI(TAG, "Pending firmware is online, confirming in %d s", CONFIG_OTBR_OTA_CONFIRM_DELAY_S);
        esp_timer_start_once(s_confirm_timer, (uint64_t)CONFIG_OTBR_OTA_CONFIRM_DELAY_S * 1000000ULL + 1);
    }
    /* Started lazily: in SoftAP setup mode there is nothing to update yet. */
    start_server();
}

static void on_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    atomic_store(&s_online, false);
    /* Only an image that stays online for the whole delay gets confirmed. */
    esp_timer_stop(s_confirm_timer);
}

/* ---------------------------------------------------------------------- */
/* HTTP client                                                             */
/* ---------------------------------------------------------------------- */

/* Opens url (following redirects) and returns a client positioned at the
 * body of a 200 response, or NULL with *why set. */
static esp_http_client_handle_t http_get(const char *url, char *why, size_t why_sz)
{
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048, /* release URLs redirect and get long */
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        snprintf(why, why_sz, "Could not create the HTTP client");
        return NULL;
    }
    int status = 0;
    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            snprintf(why, why_sz, "Could not connect to %.40s", url);
            goto fail;
        }
        if (esp_http_client_fetch_headers(client) < 0) {
            snprintf(why, why_sz, "No response from %.40s", url);
            goto fail;
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
    if (status == 200) {
        return client;
    }
    snprintf(why, why_sz, "Server answered HTTP %d", status);
fail:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return NULL;
}

static void http_done(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static int http_read(void *ctx, char *buf, int len)
{
    return esp_http_client_read((esp_http_client_handle_t)ctx, buf, len);
}

/* ---------------------------------------------------------------------- */
/* Manifest                                                                */
/* ---------------------------------------------------------------------- */

static bool copy_https_url(const cJSON *j, char *out, size_t out_sz)
{
    return cJSON_IsString(j) && strncmp(j->valuestring, "https://", 8) == 0 &&
           strlcpy(out, j->valuestring, out_sz) < out_sz;
}

static esp_err_t fetch_manifest(manifest_t *m, char *why, size_t why_sz)
{
    esp_http_client_handle_t client = http_get(CONFIG_OTBR_OTA_MANIFEST_URL, why, why_sz);
    if (client == NULL) {
        return ESP_FAIL;
    }
    char *buf = calloc(1, MANIFEST_MAX_SIZE + 1);
    if (buf == NULL) {
        http_done(client);
        snprintf(why, why_sz, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_FAIL;
    int total = 0;
    while (total < MANIFEST_MAX_SIZE) {
        int r = esp_http_client_read(client, buf + total, MANIFEST_MAX_SIZE - total);
        if (r < 0) {
            snprintf(why, why_sz, "Manifest download failed");
            goto out;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    if (total >= MANIFEST_MAX_SIZE && !esp_http_client_is_complete_data_received(client)) {
        snprintf(why, why_sz, "Manifest is too large");
        goto out;
    }

    memset(m, 0, sizeof(*m));
    cJSON *root = cJSON_Parse(buf);
    const cJSON *jver = cJSON_GetObjectItem(root, "version");
    if (!cJSON_IsString(jver) || jver->valuestring[0] == '\0' ||
        strlcpy(m->version, jver->valuestring, sizeof(m->version)) >= sizeof(m->version) ||
        !copy_https_url(cJSON_GetObjectItem(root, "url"), m->url, sizeof(m->url))) {
        snprintf(why, why_sz, "Manifest is malformed");
    } else {
        /* web_storage is optional, but must be complete if present. */
        const cJSON *jsha = cJSON_GetObjectItem(root, "storage_sha256");
        if (copy_https_url(cJSON_GetObjectItem(root, "storage_url"), m->storage_url, sizeof(m->storage_url)) &&
            cJSON_IsString(jsha) && is_sha_hex(jsha->valuestring)) {
            strlcpy(m->storage_sha, jsha->valuestring, sizeof(m->storage_sha));
        } else {
            m->storage_url[0] = '\0';
        }
        ret = ESP_OK;
    }
    cJSON_Delete(root);

out:
    free(buf);
    http_done(client);
    return ret;
}

/* Fetches the manifest and records the result. Worker task only. */
static esp_err_t do_check(void)
{
    manifest_t m;
    char why[ERROR_MAX_SIZE] = "";

    atomic_store(&s_state, STATE_CHECKING);
    esp_err_t err = fetch_manifest(&m, why, sizeof(why));
    atomic_store(&s_state, STATE_IDLE);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_check_us = esp_timer_get_time();
    if (err == ESP_OK) {
        s_manifest = m;
        s_last_error[0] = '\0';
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Latest release: %s (running %s)", m.version, esp_app_get_description()->version);
    } else {
        set_error(why);
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* Install                                                                 */
/* ---------------------------------------------------------------------- */

/* Downloads and installs the app at url and switches the boot partition.
 * Worker task only, with the busy flag held. */
static esp_err_t install_app(const char *url)
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

static esp_err_t install_storage(const manifest_t *m)
{
    char why[ERROR_MAX_SIZE] = "";
    esp_http_client_handle_t client = http_get(m->storage_url, why, sizeof(why));
    if (client == NULL) {
        set_error(why);
        return ESP_FAIL;
    }
    int64_t len = esp_http_client_get_content_length(client);
    esp_err_t err = storage_write(http_read, client, len > 0 ? (int)len : -1, m->storage_sha, why, sizeof(why));
    http_done(client);
    if (err != ESP_OK) {
        set_error(why);
    }
    return err;
}

static void reboot_now(void)
{
    atomic_store(&s_state, STATE_REBOOTING);
    ESP_LOGI(TAG, "Rebooting into the new firmware");
    vTaskDelay(pdMS_TO_TICKS(2000)); /* lets the HTTP reply and logs go out */
    esp_restart();
}

/* Whether the manifest's app should be installed. manual: a person asked. */
static bool app_wanted(const manifest_t *m, bool manual)
{
    const char *running = esp_app_get_description()->version;
    char bad[VERSION_MAX_SIZE];

    if (strcmp(m->version, running) == 0) {
        return false;
    }
    if (manual) {
        /* Install whatever the manifest names; allows going back to it. */
        return true;
    }
    if (version_compare(m->version, running) <= 0) {
        return false;
    }
    if (last_invalid_version(bad, sizeof(bad)) && strcmp(bad, m->version) == 0) {
        ESP_LOGW(TAG, "Not installing %s automatically: it rolled back before", m->version);
        return false;
    }
    return true;
}

/*
 * Brings the device to the manifest's release: the app if wanted, then
 * web_storage if it differs. web_storage is only written alongside the app
 * it belongs to, or when that app is already running, so the UI never runs
 * ahead of the REST API serving it. Worker task only, busy flag held.
 */
static void install_release(bool manual)
{
    manifest_t m;
    get_manifest(&m);
    if (m.version[0] == '\0') {
        return;
    }
    if (running_image_pending()) {
        /* Installing now would overwrite the slot we would roll back to. */
        if (manual) {
            set_error("The running firmware is not confirmed yet; try again in a minute");
        }
        return;
    }

    bool app = app_wanted(&m, manual);
    bool storage = storage_differs(&m) && (app || strcmp(m.version, esp_app_get_description()->version) == 0);
    if (!app && !storage) {
        if (manual) {
            set_error("Already up to date");
        }
        return;
    }

    if (app && install_app(m.url) != ESP_OK) {
        return;
    }
    if (storage && install_storage(&m) != ESP_OK && !app) {
        return; /* retried at the next check */
    }
    if (app) {
        reboot_now();
    }
}

/* ---------------------------------------------------------------------- */
/* Worker                                                                  */
/* ---------------------------------------------------------------------- */

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
        next_auto = xTaskGetTickCount() +
                    pdMS_TO_TICKS((err == ESP_OK ? CONFIG_OTBR_OTA_CHECK_INTERVAL_HOURS : 1) * HOUR_MS);
        if (err == ESP_OK) {
            if (bits & REQ_INSTALL) {
                install_release(true);
            } else if (!manual && atomic_load(&s_auto)) {
                install_release(false);
            }
        }
        release();
    }
}

static void notify_worker(uint32_t bits)
{
    if (s_worker) {
        xTaskNotify(s_worker, bits, eSetBits);
    }
}

/* ---------------------------------------------------------------------- */
/* HTTP server                                                             */
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
    manifest_t m;
    char error[ERROR_MAX_SIZE], bad[VERSION_MAX_SIZE], storage_sha[SHA_HEX_SIZE];
    int64_t last_check;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    m = s_manifest;
    strlcpy(error, s_last_error, sizeof(error));
    strlcpy(storage_sha, s_storage_sha, sizeof(storage_sha));
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
    cJSON_AddStringToObject(root, "storage_sha256", storage_sha);
    if (m.version[0]) {
        cJSON_AddStringToObject(root, "latest", m.version);
        /* Undecidable comparisons (a dev build) count as "different". */
        int cmp = version_compare(m.version, app->version);
        bool app_newer = cmp > 0 || (cmp == 0 && strcmp(m.version, app->version) != 0);
        bool storage = storage_differs(&m) && strcmp(m.version, app->version) == 0;
        cJSON_AddBoolToObject(root, "update_available", app_newer || storage);
    }
    if (last_check) {
        cJSON_AddNumberToObject(root, "last_check_s_ago", (double)((esp_timer_get_time() - last_check) / 1000000));
    }
    if (last_invalid_version(bad, sizeof(bad))) {
        cJSON_AddStringToObject(root, "rolled_back_from", bad);
    }
    cJSON_AddBoolToObject(root, "coredump", esp_core_dump_image_check() == ESP_OK);
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
                            "Installing the latest release. The device reboots if the firmware changes.");
}

/* Reads a short request body into buf (NUL-terminated). */
static bool recv_small_body(httpd_req_t *req, char *buf, size_t buf_sz)
{
    if (req->content_len == 0 || req->content_len >= buf_sz) {
        return false;
    }
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    buf[got] = '\0';
    return true;
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
    if (!guard_post(req)) {
        return ESP_OK;
    }
    char body[16] = "";
    if (!recv_small_body(req, body, sizeof(body))) {
        return send_error(req, "400 Bad Request", "Body must be \"auto\" or \"manual\"");
    }
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

/* httpd_req_recv with a few timeouts tolerated; read_fn_t-compatible. */
static int req_read(void *ctx, char *buf, int len)
{
    httpd_req_t *req = ctx;
    for (int timeouts = 0; timeouts <= 3; timeouts++) {
        int r = httpd_req_recv(req, buf, len);
        if (r != HTTPD_SOCK_ERR_TIMEOUT) {
            return r;
        }
    }
    return -1;
}

static int req_read_full(httpd_req_t *req, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = req_read(req, buf + got, len - got);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    return got;
}

/* Common preconditions for both uploads; claims the busy flag on success. */
static bool upload_begin(httpd_req_t *req)
{
    if (!guard_post(req)) {
        return false;
    }
    if (running_image_pending()) {
        send_error(req, "409 Conflict", "The running firmware is not confirmed yet; try again in a minute");
        return false;
    }
    if (!try_claim()) {
        send_error(req, "409 Conflict", "An update is already in progress");
        return false;
    }
    set_error(NULL);
    return true;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
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
    if (!upload_begin(req)) {
        return ESP_OK;
    }

    char *buf = malloc(CHUNK_SIZE);
    if (buf == NULL) {
        release();
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    atomic_store(&s_state, STATE_UPLOADING);
    atomic_store(&s_progress, 0);
    ESP_LOGI(TAG, "Receiving upload (%d bytes) into %s", remaining, target->label);

    const int total = remaining;
    char why[ERROR_MAX_SIZE] = "";
    esp_ota_handle_t ota = 0;
    esp_err_t err = ESP_OK;

    /* Check the header before erasing anything. */
    int len = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
    if (req_read_full(req, buf, len) != len) {
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
        len = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        if (req_read_full(req, buf, len) != len) {
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

static esp_err_t upload_storage_post_handler(httpd_req_t *req)
{
    if (s_storage == NULL) {
        return send_error(req, "500 Internal Server Error", "No web_storage partition");
    }
    if (req->content_len != s_storage->size) {
        return send_error(req, "400 Bad Request", "Upload must be web_storage.bin from the same release");
    }
    if (!upload_begin(req)) {
        return ESP_OK;
    }
    char why[ERROR_MAX_SIZE] = "";
    esp_err_t err = storage_write(req_read, req, req->content_len, NULL, why, sizeof(why));
    release();
    if (err != ESP_OK) {
        set_error(why);
        return send_error(req, "400 Bad Request", why);
    }
    return send_json_status(req, NULL, "status", "Web storage updated.");
}

/* Streams the stored crash dump (ELF). Read it with
 * `idf.py coredump-info --core <file>` against the matching build. */
static esp_err_t coredump_get_handler(httpd_req_t *req)
{
    size_t addr = 0, size = 0;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (part == NULL || esp_core_dump_image_check() != ESP_OK ||
        esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return send_error(req, "404 Not Found", "No crash dump stored");
    }
    char *buf = malloc(CHUNK_SIZE);
    if (buf == NULL) {
        return send_error(req, "500 Internal Server Error", "Out of memory");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.elf\"");
    size_t offset = addr - part->address;
    esp_err_t err = ESP_OK;
    for (size_t done = 0; done < size && err == ESP_OK; done += CHUNK_SIZE) {
        size_t n = size - done < CHUNK_SIZE ? size - done : CHUNK_SIZE;
        err = esp_partition_read(part, offset + done, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, n);
        }
    }
    free(buf);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t coredump_delete_handler(httpd_req_t *req)
{
    if (!guard_post(req)) {
        return ESP_OK;
    }
    esp_core_dump_image_erase();
    return send_json_status(req, NULL, "status", "Crash dump erased.");
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
    config.max_uri_handlers = 10;
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
        {.uri = "/api/ota/upload-storage", .method = HTTP_POST, .handler = upload_storage_post_handler},
        {.uri = "/api/ota/mode", .method = HTTP_POST, .handler = mode_post_handler},
        {.uri = "/api/coredump", .method = HTTP_GET, .handler = coredump_get_handler},
        {.uri = "/api/coredump", .method = HTTP_DELETE, .handler = coredump_delete_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    ESP_LOGI(TAG, "Update server on port %d", CONFIG_OTBR_OTA_HTTP_PORT);
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

    /* Hash before mounting, so the value reflects exactly what was flashed. */
    s_storage = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, STORAGE_LABEL);
    if (s_storage) {
        storage_hash_update();
        storage_mount(); /* failure is logged; an update can repair it */
    } else {
        ESP_LOGE(TAG, "No %s partition: the web UI will not load", STORAGE_LABEL);
    }

    if (esp_core_dump_image_check() == ESP_OK) {
        ESP_LOGW(TAG, "A crash dump is stored: GET /api/coredump on port %d", CONFIG_OTBR_OTA_HTTP_PORT);
    }

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

    /* Large stack: TLS handshake plus esp_https_ota. */
    if (xTaskCreate(worker_task, "otbr_ota", 8192, NULL, 4, &s_worker) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
