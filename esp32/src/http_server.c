#include "http_server.h"
#include "wifi_manager.h"
#include "html.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "http"
#define PORTAL_REDIRECT "http://" AP_IP_STR "/"

static const char *s_networks_html = NULL;

// ── URL helpers ───────────────────────────────────────────────────────────────

static void urldecode(char *dst, size_t dstlen, const char *src) {
    size_t i = 0;
    while (*src && i < dstlen - 1) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) &&
                           isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void get_field(const char *body, const char *key, char *out, size_t outlen) {
    out[0] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) return;
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= sizeof((char[256]){})) len = 254;
    char raw[256];
    memcpy(raw, p, len);
    raw[len] = '\0';
    urldecode(out, outlen, raw);
}

// Escapes <, >, &, ", ' for safe HTML embedding of user-supplied strings.
static void html_escape(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    for (; *src && i + 7 < dstsz; src++) {
        switch (*src) {
            case '<':  memcpy(dst+i, "&lt;",   4); i += 4; break;
            case '>':  memcpy(dst+i, "&gt;",   4); i += 4; break;
            case '&':  memcpy(dst+i, "&amp;",  5); i += 5; break;
            case '"':  memcpy(dst+i, "&quot;", 6); i += 6; break;
            case '\'': memcpy(dst+i, "&#39;",  5); i += 5; break;
            default:   dst[i++] = *src; break;
        }
    }
    dst[i] = '\0';
}

// ── Shared redirect helper ────────────────────────────────────────────────────

static esp_err_t redirect_to_portal(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", PORTAL_REDIRECT);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ── Setup mode handlers ───────────────────────────────────────────────────────

static esp_err_t setup_root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, SETUP_BEFORE_NETS, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, s_networks_html,   HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, SETUP_AFTER_NETS,  HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t setup_save_handler(httpd_req_t *req) {
    char body[512] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[n] = '\0';

    char ssid[64] = {0}, ssid_manual[64] = {0}, pass[64] = {0};
    get_field(body, "ssid",        ssid,        sizeof(ssid));
    get_field(body, "ssid_manual", ssid_manual, sizeof(ssid_manual));
    get_field(body, "pass",        pass,        sizeof(pass));

    // Manual entry overrides dropdown (supports hidden networks).
    if (ssid_manual[0] != '\0') {
        strlcpy(ssid, ssid_manual, sizeof(ssid));
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    wifi_save_credentials(ssid, pass);
    ESP_LOGI(TAG, "Saved: ssid=%s — rebooting", ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVING_HTML, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t setup_catchall_handler(httpd_req_t *req) {
    return redirect_to_portal(req);
}

// ── Portal mode handlers ──────────────────────────────────────────────────────

static esp_err_t portal_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_WELCOME_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handles the "Grow a Leaf" form POST.
static esp_err_t portal_post_handler(httpd_req_t *req) {
    char body[256] = {0};
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n > 0) body[n] = '\0';

    char name[64] = {0};
    get_field(body, "name", name, sizeof(name));
    if (name[0] == '\0') {
        strncpy(name, "friend", sizeof(name) - 1);
    }
    name[40] = '\0'; // spec max

    char safe_name[320]; // max 40 chars × 6 bytes worst-case HTML escaping + NUL
    html_escape(safe_name, sizeof(safe_name), name);

    ESP_LOGI(TAG, "leaf grown: %s", name);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, PORTAL_SUCCESS_A,  HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, safe_name,          HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, PORTAL_SUCCESS_B,  HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t portal_catchall_handler(httpd_req_t *req) {
    return redirect_to_portal(req);
}

// ── Server start ──────────────────────────────────────────────────────────────

void http_server_start_setup(const char *networks_html) {
    s_networks_html = networks_html;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = setup_root_handler };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = setup_save_handler };
    httpd_uri_t wild = { .uri = "/*",    .method = HTTP_GET,  .handler = setup_catchall_handler };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &wild);

    ESP_LOGI(TAG, "Setup wizard HTTP server ready");
}

void http_server_start_portal(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;

    httpd_handle_t server;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t get_root  = { .uri = "/",  .method = HTTP_GET,  .handler = portal_get_handler };
    httpd_uri_t post_root = { .uri = "/",  .method = HTTP_POST, .handler = portal_post_handler };
    httpd_uri_t get_wild  = { .uri = "/*", .method = HTTP_GET,  .handler = portal_catchall_handler };
    httpd_uri_t post_wild = { .uri = "/*", .method = HTTP_POST, .handler = portal_catchall_handler };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_root);
    httpd_register_uri_handler(server, &get_wild);
    httpd_register_uri_handler(server, &post_wild);

    ESP_LOGI(TAG, "Captive portal HTTP server ready");
}
