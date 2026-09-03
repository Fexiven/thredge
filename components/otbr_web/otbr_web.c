/*
 * Web UI support for the ESP32-C5 OpenThread Border Router.
 *
 * The border router component (esp_ot_br_server) provides the Thread REST API.
 * This component serves the project's own UI from web_ui/, embedded in the
 * binary so that an OTA update carries the pages with it, plus the Wi-Fi
 * endpoints upstream does not have, since its Wi-Fi handling only covers
 * first-boot SoftAP setup.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "otbr_web.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_br_web.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_ot_wifi_cmd.h"
#include "esp_wifi_types.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "otbr_ota.h"

#define TAG "otbr_web"

#define BODY_MAX_SIZE 512

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_cjson(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Out of memory\"}");
    }
    esp_err_t err = send_json(req, out);
    cJSON_free(out);
    return err;
}

/* 2.4 GHz channels are 1..14; everything above is 5 GHz. The C5 is dual band,
 * so the UI shows which one each network is on. */
static const char *band_of_channel(int channel)
{
    return channel > 14 ? "5 GHz" : "2.4 GHz";
}

static const char *authmode_name(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA3";
    default:
        return "secured";
    }
}

/* ---------------------------------------------------------------------- */
/* Embedded UI                                                             */
/* ---------------------------------------------------------------------- */

#define EMBEDDED(sym)                                    \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[] asm("_binary_" #sym "_end")

EMBEDDED(index_html);
EMBEDDED(wifi_html);
EMBEDDED(thread_html);
EMBEDDED(map_html);
EMBEDDED(update_html);
EMBEDDED(style_css);
EMBEDDED(app_js);

typedef struct {
    const char *uri;
    const char *type;
    const uint8_t *start;
    const uint8_t *end;
} embedded_page_t;

static const embedded_page_t s_pages[] = {
    {"/", "text/html", index_html_start, index_html_end},
    {"/index.html", "text/html", index_html_start, index_html_end},
    {"/wifi.html", "text/html", wifi_html_start, wifi_html_end},
    {"/thread.html", "text/html", thread_html_start, thread_html_end},
    {"/map.html", "text/html", map_html_start, map_html_end},
    {"/update.html", "text/html", update_html_start, update_html_end},
    {"/static/style.css", "text/css", style_css_start, style_css_end},
    {"/static/app.js", "application/javascript", app_js_start, app_js_end},
};

static esp_err_t page_get_handler(httpd_req_t *req)
{
    /* httpd hands over the query string as part of the URI. */
    const char *q = strchr(req->uri, '?');
    size_t len = q ? (size_t)(q - req->uri) : strlen(req->uri);

    for (size_t i = 0; i < sizeof(s_pages) / sizeof(s_pages[0]); i++) {
        if (strlen(s_pages[i].uri) == len && strncmp(req->uri, s_pages[i].uri, len) == 0) {
            httpd_resp_set_type(req, s_pages[i].type);
            return httpd_resp_send(req, (const char *)s_pages[i].start,
                                   s_pages[i].end - s_pages[i].start);
        }
    }
    /* Answered here rather than by the border router's fallback, which has no
     * filesystem to read from now. Browsers ask for this on every page load. */
    if (len == strlen("/favicon.ico") && strncmp(req->uri, "/favicon.ico", len) == 0) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Not found", HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------------------------------------------- */
/* Wi-Fi status                                                            */
/* ---------------------------------------------------------------------- */

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Out of memory\"}");
    }

    wifi_ap_record_t ap;
    bool connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    cJSON_AddBoolToObject(root, "connected", connected);

    if (connected) {
        char ssid[33] = "";
        memcpy(ssid, ap.ssid, sizeof(ap.ssid));
        ssid[32] = '\0';
        cJSON_AddStringToObject(root, "ssid", ssid);
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddNumberToObject(root, "channel", ap.primary);
        cJSON_AddStringToObject(root, "band", band_of_channel(ap.primary));
        cJSON_AddStringToObject(root, "security", authmode_name(ap.authmode));
    } else {
        /* Not associated: still show what is stored, so the UI can say which
         * network it is trying to reach. */
        char stored[64] = "";
        if (esp_ot_wifi_config_get_ssid(stored) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", stored);
        }
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(root, "ip", buf);
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.gw));
        cJSON_AddStringToObject(root, "gateway", buf);
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(root, "mac", buf);
    }

    return send_cjson(req, root);
}

/* ---------------------------------------------------------------------- */
/* Wi-Fi credentials                                                       */
/* ---------------------------------------------------------------------- */

static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > BODY_MAX_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"Unexpected request size\"}");
    }

    char body[BODY_MAX_SIZE + 1];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"Could not read the request\"}");
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"Malformed request\"}");
    }
    const cJSON *jssid = cJSON_GetObjectItem(root, "ssid");
    const cJSON *jpass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(jssid) || jssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"A network name is required\"}");
    }
    const char *password = cJSON_IsString(jpass) ? jpass->valuestring : "";

    esp_err_t err = esp_ot_wifi_config_set_ssid(jssid->valuestring);
    if (err == ESP_OK) {
        err = esp_ot_wifi_config_set_password(password);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not store Wi-Fi credentials: %s", esp_err_to_name(err));
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Could not save the credentials\"}");
    }

    ESP_LOGI(TAG, "Wi-Fi credentials updated, reconnecting to %s", jssid->valuestring);
    /* Answer before tearing down the connection, or the reply never arrives:
     * the reconnect drops the socket this response is travelling on. */
    esp_err_t sent = send_json(req, "{\"status\":\"Saved. Reconnecting -- this page may briefly go offline.\"}");

    esp_ot_wifi_disconnect();
    esp_ot_wifi_connect(jssid->valuestring, password);
    cJSON_Delete(root);
    return sent;
}


/* ---------------------------------------------------------------------- */
/* Band preference                                                         */
/* ---------------------------------------------------------------------- */

/* Stored preference; applied on every STA_START so it survives reboots.
 * WIFI_BAND_MODE_AUTO leaves things to the driver, which is the current C5
 * default -- both bands allowed and the AP steers. */

#define BAND_NVS_NAMESPACE "otbr_web"
#define BAND_NVS_KEY "band"

static wifi_band_mode_t s_band_pref = WIFI_BAND_MODE_AUTO;

static const char *band_mode_name(wifi_band_mode_t m)
{
    switch (m) {
    case WIFI_BAND_MODE_5G_ONLY: return "5g_only";
    case WIFI_BAND_MODE_2G_ONLY: return "2g_only";
    default:                     return "auto";
    }
}

static bool band_mode_parse(const char *s, wifi_band_mode_t *out)
{
    if (!s) { return false; }
    if (!strcmp(s, "auto"))    { *out = WIFI_BAND_MODE_AUTO;    return true; }
    if (!strcmp(s, "5g_only")) { *out = WIFI_BAND_MODE_5G_ONLY; return true; }
    if (!strcmp(s, "2g_only")) { *out = WIFI_BAND_MODE_2G_ONLY; return true; }
    return false;
}

static void band_pref_load(void)
{
    nvs_handle_t h;
    if (nvs_open(BAND_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return; /* first boot: leave at AUTO */
    }
    uint8_t v = WIFI_BAND_MODE_AUTO;
    if (nvs_get_u8(h, BAND_NVS_KEY, &v) == ESP_OK) {
        s_band_pref = (wifi_band_mode_t)v;
    }
    nvs_close(h);
}

static esp_err_t band_pref_save(wifi_band_mode_t m)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BAND_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { return err; }
    err = nvs_set_u8(h, BAND_NVS_KEY, (uint8_t)m);
    if (err == ESP_OK) { err = nvs_commit(h); }
    nvs_close(h);
    return err;
}

/* Fires on every esp_wifi_start(); no need to disconnect first at boot,
 * because Wi-Fi has not associated yet. */
static void on_sta_start(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_err_t err = esp_wifi_set_band_mode(s_band_pref);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Band mode applied: %s", band_mode_name(s_band_pref));
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Could not apply band mode %s: %s",
                 band_mode_name(s_band_pref), esp_err_to_name(err));
    }
}

esp_err_t otbr_web_init(void)
{
    band_pref_load();
    return esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                      on_sta_start, NULL);
}

static esp_err_t wifi_band_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Out of memory\"}");
    }
    cJSON_AddStringToObject(root, "preference", band_mode_name(s_band_pref));
    /* Effective band shown so the UI can say "the AP put you on 2.4 GHz
     * despite auto" rather than only echoing the preference. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(root, "current_band", band_of_channel(ap.primary));
        cJSON_AddNumberToObject(root, "current_channel", ap.primary);
    }
    return send_cjson(req, root);
}

static esp_err_t wifi_band_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > BODY_MAX_SIZE) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"Unexpected request size\"}");
    }
    char body[BODY_MAX_SIZE + 1];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"Could not read the request\"}");
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    const cJSON *jmode = root ? cJSON_GetObjectItem(root, "mode") : NULL;
    wifi_band_mode_t mode;
    if (!cJSON_IsString(jmode) || !band_mode_parse(jmode->valuestring, &mode)) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"error\":\"mode must be auto, 5g_only or 2g_only\"}");
    }
    cJSON_Delete(root);

    if (mode == s_band_pref) {
        return send_json(req, "{\"status\":\"No change.\"}");
    }

    esp_err_t err = band_pref_save(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist band preference: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"Could not save the preference\"}");
    }
    s_band_pref = mode;
    ESP_LOGI(TAG, "Band preference set to %s, reconnecting", band_mode_name(mode));

    /* Answer before tearing down the association, or the reply never arrives:
     * the reconnect drops the socket this response travels on. Same pattern
     * as changing Wi-Fi credentials. */
    esp_err_t sent = send_json(req,
        "{\"status\":\"Saved. Reconnecting -- this page may briefly go offline.\"}");

    /* Grab the credentials to reconnect with, then bring the link down,
     * change bands and bring it back up. */
    char ssid[64] = "", pass[64] = "";
    if (esp_ot_wifi_config_get_ssid(ssid) == ESP_OK) {
        esp_ot_wifi_config_get_password(pass);
    }
    esp_ot_wifi_disconnect();
    esp_wifi_set_band_mode(mode); /* now safe: no active association */
    if (ssid[0]) {
        esp_ot_wifi_connect(ssid, pass);
    }
    return sent;
}

/* ---------------------------------------------------------------------- */
/* Registration                                                            */
/* ---------------------------------------------------------------------- */

/* Overrides the weak no-op in esp_br_web.c. Called once the border router web
 * server has registered its own routes and before its static-file catch-all,
 * so these match first. */
void esp_br_web_register_extra_uris(httpd_handle_t server)
{

    const httpd_uri_t status = {.uri = "/api/wifi/status", .method = HTTP_GET, .handler = wifi_status_get_handler};
    const httpd_uri_t config = {.uri = "/api/wifi/config", .method = HTTP_POST, .handler = wifi_config_post_handler};
    const httpd_uri_t band_get = {.uri = "/api/wifi/band", .method = HTTP_GET, .handler = wifi_band_get_handler};
    const httpd_uri_t band_post = {.uri = "/api/wifi/band", .method = HTTP_POST, .handler = wifi_band_post_handler};
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &config);
    httpd_register_uri_handler(server, &band_get);
    httpd_register_uri_handler(server, &band_post);

    otbr_ota_register_uris(server);

    /* The page catch-all is registered LAST. With wildcard matching the first
     * registered handler whose pattern matches wins, so the wildcard would
     * otherwise shadow the GET API routes above (api/ota/version and
     * api/ota/check) and answer them from the page table -- a 404. It also
     * claims the wildcard before esp_ot_br_server registers its own
     * static-file fallback, which fails with ESP_ERR_HTTPD_HANDLER_EXISTS and
     * logs a line at boot: deliberate, since that fallback reads from the
     * SPIFFS partition this project no longer has. */
    const httpd_uri_t pages = {.uri = "/*", .method = HTTP_GET, .handler = page_get_handler};
    httpd_register_uri_handler(server, &pages);

    ESP_LOGI(TAG, "Web UI and Wi-Fi API registered");
}
