#include "http_server.h"
#include "wifi_manager.h"
#include "client_state.h"
#include "config.h"
#include "authz.h"
#include "shaper.h"
#include "html.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG "http"
#define PORTAL_REDIRECT "http://" AP_IP_STR "/"

// ── Admin sessions ────────────────────────────────────────────────────────────
#define ADMIN_SESS_TTL_US (3600LL * 1000000)  // 1 hour
#define MAX_SESS          4

static struct { char tok[33]; int64_t exp_us; } s_sess[MAX_SESS];

static void rand_hex(char *out, int nbytes) {
    static const char *H = "0123456789abcdef";
    unsigned char rnd[16];
    if (nbytes > (int)sizeof(rnd)) nbytes = sizeof(rnd);
    esp_fill_random(rnd, nbytes);
    for (int i = 0; i < nbytes; i++) {
        out[i * 2]     = H[rnd[i] >> 4];
        out[i * 2 + 1] = H[rnd[i] & 0xF];
    }
    out[nbytes * 2] = '\0';
}

static void admin_new_session(char tok_out[33]) {
    rand_hex(tok_out, 16);
    int64_t now = esp_timer_get_time();
    int slot = 0;
    for (int s = 0; s < MAX_SESS; s++) {
        if (s_sess[s].exp_us <= now) { slot = s; break; }
        if (s_sess[s].exp_us < s_sess[slot].exp_us) slot = s;
    }
    strcpy(s_sess[slot].tok, tok_out);
    s_sess[slot].exp_us = now + ADMIN_SESS_TTL_US;
}

// Extract the wt_admin cookie token into tok (33 bytes). Returns false if absent.
static bool admin_cookie(httpd_req_t *req, char tok[33]) {
    char cookie[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK)
        return false;
    char *p = strstr(cookie, "wt_admin=");
    if (!p) return false;
    p += strlen("wt_admin=");
    int i = 0;
    while (p[i] && p[i] != ';' && p[i] != ' ' && i < 32) { tok[i] = p[i]; i++; }
    tok[i] = '\0';
    return i > 0;
}

static bool admin_authed(httpd_req_t *req) {
    char tok[33];
    if (!admin_cookie(req, tok)) return false;
    int64_t now = esp_timer_get_time();
    for (int s = 0; s < MAX_SESS; s++)
        if (s_sess[s].exp_us > now && strcmp(s_sess[s].tok, tok) == 0) return true;
    return false;
}

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

    char ssid[64] = {0}, ssid_manual[64] = {0}, pass[64] = {0}, admin_pass[64] = {0};
    get_field(body, "ssid",        ssid,        sizeof(ssid));
    get_field(body, "ssid_manual", ssid_manual, sizeof(ssid_manual));
    get_field(body, "pass",        pass,        sizeof(pass));
    get_field(body, "admin_pass",  admin_pass,  sizeof(admin_pass));

    // Manual entry overrides dropdown (supports hidden networks).
    if (ssid_manual[0] != '\0') {
        strlcpy(ssid, ssid_manual, sizeof(ssid));
    }

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    if (admin_pass[0] != '\0') config_set_admin_password(admin_pass);

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

// Resolve the AP-side IPv4 address (network byte order) behind a request, or 0.
static uint32_t client_ip(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return 0;
    struct sockaddr_in6 a6;
    socklen_t len = sizeof(a6);
    if (getpeername(fd, (struct sockaddr *)&a6, &len) != 0) return 0;
    if (a6.sin6_family == AF_INET)
        return ((struct sockaddr_in *)&a6)->sin_addr.s_addr;
    // IPv4-mapped IPv6 (::ffff:a.b.c.d) — the v4 address is the last 4 bytes.
    uint32_t ip;
    memcpy(&ip, &a6.sin6_addr.s6_addr[12], 4);
    return ip;
}

// Renders the "Active leaf" status card for a returning visitor.
static esp_err_t send_status_card(httpd_req_t *req, client_t *c, int secs_left) {
    char safe_name[256], safe_host[200];
    html_escape(safe_name, sizeof(safe_name), c->name[0] ? c->name : "friend");
    html_escape(safe_host, sizeof(safe_host), c->hostname);

    int h = secs_left / 3600, m = (secs_left % 3600) / 60;
    char fresh[64];
    if (h > 0) snprintf(fresh, sizeof(fresh), "%dh %dm", h, m);
    else       snprintf(fresh, sizeof(fresh), "%dm", m);

    char body[1024];
    int len = snprintf(body, sizeof(body),
        "<div class='card ok'>"
        "<p class='headline'>Your leaf is still fresh, %s &#x1F33F;</p>"
        "<p class='sub'>You're online. About <strong>%s</strong> of freshness left.</p>"
        "%s%s%s"
        "</div>"
        "<a class='btnlink' href='http://wifi.tree'>Keep browsing &rarr;</a>",
        safe_name, fresh,
        safe_host[0] ? "<p class='sub' style='opacity:.6'>device: " : "",
        safe_host[0] ? safe_host : "",
        safe_host[0] ? "</p>" : "");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, PORTAL_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, body, len);
    httpd_resp_send_chunk(req, PORTAL_FOOT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t portal_get_handler(httpd_req_t *req) {
    int ttl = config_leaf_ttl_seconds();
    client_t *c = clients_find_by_ip(client_ip(req));
    if (client_leaf_active(c, ttl))
        return send_status_card(req, c, client_leaf_seconds_left(c, ttl));

    // New visitor, or leaf expired — show the grow-a-leaf welcome form.
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

    // Record the leaf against this client so revisits show the status card,
    // and open the internet gate for this IP until the leaf expires.
    uint32_t ip = client_ip(req);
    client_t *c = clients_find_by_ip(ip);
    if (c) clients_grow_leaf(c, name);
    if (ip) {
        int ttl = config_leaf_ttl_seconds();
        int64_t expiry = ttl <= 0 ? 0 : esp_timer_get_time() + (int64_t)ttl * 1000000;
        authz_grant(ip, expiry);
    }

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

// ── Admin page ────────────────────────────────────────────────────────────────

static esp_err_t redirect_to(httpd_req_t *req, const char *loc, const char *cookie) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    if (cookie) httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t send_admin_page(httpd_req_t *req, const char *body, int len) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, PORTAL_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, body, len);
    httpd_resp_send_chunk(req, PORTAL_FOOT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t admin_dashboard(httpd_req_t *req) {
    int ttl = config_leaf_ttl_seconds();

    // Heap-allocate the large buffers — too big for the httpd task stack.
    const int BODY_SZ = 12288;
    client_t *list = malloc(sizeof(client_t) * 16);
    char *body = malloc(BODY_SZ);
    if (!list || !body) {
        free(list); free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    int n = clients_snapshot(list, 16);

    int o = snprintf(body, BODY_SZ,
        "<div class='card'>"
        "<p class='headline' style='color:#9fe89f'>Admin &#x1F510;</p>"
        "<p class='sub'>Uplink: <strong>%s</strong> &middot; %d device(s) connected</p>"
        "</div>"
        "<div class='card'>"
        "<p class='headline' style='font-size:1em'>Settings</p>"
        "<form method='POST' action='/admin/settings'>"
        "<label style='font-size:13px;opacity:.7'>Leaf freshness (minutes, 0 = never)</label>"
        "<input type='number' name='ttl_min' min='0' value='%d'>"
        "<label style='font-size:13px;opacity:.7'>Per-device speed (kbps, 0 = uncapped)</label>"
        "<input type='number' name='kbps' min='0' value='%d'>"
        "<button type='submit'>Save</button>"
        "</form>"
        "<p class='note'>Each visitor is capped to this speed up and down.</p>"
        "</div>"
        "<div class='card'>"
        "<p class='headline' style='font-size:1em'>Visitors this session</p>",
        wifi_has_uplink() ? "connected" : "connecting…",
        clients_count(), ttl / 60, config_client_kbps());

    for (int i = 0; i < n && o < BODY_SZ - 800; i++) {
        char nm[128], hn[128];
        html_escape(nm, sizeof(nm), list[i].name[0] ? list[i].name : "(no name yet)");
        html_escape(hn, sizeof(hn), list[i].hostname);
        int left = client_leaf_seconds_left(&list[i], ttl);
        char status[48];
        if (list[i].leaf_grown_us == 0)       snprintf(status, sizeof(status), "no leaf");
        else if (!client_leaf_active(&list[i], ttl)) snprintf(status, sizeof(status), "expired");
        else if (ttl <= 0)                    snprintf(status, sizeof(status), "fresh");
        else snprintf(status, sizeof(status), "%dh %dm left", left / 3600, (left % 3600) / 60);

        o += snprintf(body + o, BODY_SZ - o,
            "<div style='border-top:1px solid #1f3d29;padding:10px 0'>"
            "<p class='sub' style='margin:0 0 8px'>&#x1F33F; <strong>%s</strong>"
            "%s%s <span style='opacity:.6'>&middot; %s</span></p>",
            nm, hn[0] ? " &middot; " : "", hn, status);

        if (list[i].ip) {  // controls need a known IP to target
            unsigned long ip = (unsigned long)list[i].ip;
            o += snprintf(body + o, BODY_SZ - o,
                "<div style='display:flex;gap:6px'>"
                "<form method='POST' action='/admin/userspeed' "
                "style='display:flex;gap:6px;margin:0;flex:1'>"
                "<input type='hidden' name='ip' value='%lu'>"
                "<input type='number' name='kbps' min='0' placeholder='kbps' "
                "style='margin:0;flex:1'>"
                "<button style='margin:0;padding:10px 12px'>Set</button></form>"
                "<form method='POST' action='/admin/kick' style='margin:0'>"
                "<input type='hidden' name='ip' value='%lu'>"
                "<button style='margin:0;padding:10px 14px;background:#aa3333'>Kick</button>"
                "</form></div>",
                ip, ip);
        }
        o += snprintf(body + o, BODY_SZ - o, "</div>");
    }
    if (n == 0)
        o += snprintf(body + o, BODY_SZ - o,
            "<p class='sub' style='opacity:.6'>Nobody yet.</p>");

    o += snprintf(body + o, BODY_SZ - o,
        "</div>"
        "<form method='POST' action='/admin/logout'>"
        "<button type='submit' style='background:#1f3d29'>Log out</button></form>");

    if (o > BODY_SZ - 1) o = BODY_SZ - 1; // snprintf returns would-be length; clamp
    esp_err_t r = send_admin_page(req, body, o);
    free(list);
    free(body);
    return r;
}

static esp_err_t admin_get_handler(httpd_req_t *req) {
    char body[700];
    if (!config_has_admin_password()) {
        int len = snprintf(body, sizeof(body),
            "<div class='card'>"
            "<p class='headline'>Set an admin password &#x1F510;</p>"
            "<p class='sub'>Protects <strong>wifi.tree/admin</strong>. "
            "Set this before anyone joins.</p>"
            "<form method='POST' action='/admin/setpw'>"
            "<input type='password' name='pw' placeholder='new admin password' required>"
            "<button type='submit'>Save password</button>"
            "</form></div>");
        return send_admin_page(req, body, len);
    }
    if (!admin_authed(req)) {
        int len = snprintf(body, sizeof(body),
            "<div class='card'>"
            "<p class='headline'>Admin &#x1F510;</p>"
            "<form method='POST' action='/admin/login'>"
            "<input type='password' name='pw' placeholder='admin password' required>"
            "<button type='submit'>Log in</button>"
            "</form></div>");
        return send_admin_page(req, body, len);
    }
    return admin_dashboard(req);
}

static esp_err_t admin_setpw_handler(httpd_req_t *req) {
    if (config_has_admin_password()) return redirect_to(req, "/admin", NULL);
    char rbody[256] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char pw[64] = {0};
    get_field(rbody, "pw", pw, sizeof(pw));
    if (pw[0]) {
        config_set_admin_password(pw);
        ESP_LOGI(TAG, "admin password set");
    }
    return redirect_to(req, "/admin", NULL);
}

static esp_err_t admin_login_handler(httpd_req_t *req) {
    char rbody[256] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char pw[64] = {0};
    get_field(rbody, "pw", pw, sizeof(pw));
    if (!config_check_admin_password(pw))
        return redirect_to(req, "/admin", NULL);

    char tok[33], cookie[96];
    admin_new_session(tok);
    snprintf(cookie, sizeof(cookie),
             "wt_admin=%s; Path=/; Max-Age=3600; HttpOnly; SameSite=Strict", tok);
    return redirect_to(req, "/admin", cookie);
}

static esp_err_t admin_settings_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[256] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char ttl[16] = {0}, kbps[16] = {0};
    get_field(rbody, "ttl_min", ttl, sizeof(ttl));
    get_field(rbody, "kbps",    kbps, sizeof(kbps));
    if (ttl[0])  config_set_leaf_ttl_seconds(atoi(ttl) * 60);
    if (kbps[0]) config_set_client_kbps(atoi(kbps));
    return redirect_to(req, "/admin", NULL);
}

// Cut a single visitor off: revoke their internet grant and clear their leaf.
static esp_err_t admin_kick_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char ips[16] = {0};
    get_field(rbody, "ip", ips, sizeof(ips));
    uint32_t ip = (uint32_t)strtoul(ips, NULL, 10);
    if (ip) {
        authz_revoke(ip);
        clients_clear_leaf_by_ip(ip);
        ESP_LOGI(TAG, "admin kicked %lu", (unsigned long)ip);
    }
    return redirect_to(req, "/admin", NULL);
}

// Override one visitor's bandwidth cap (kbps; 0 = uncapped).
static esp_err_t admin_userspeed_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char ips[16] = {0}, kb[16] = {0};
    get_field(rbody, "ip",   ips, sizeof(ips));
    get_field(rbody, "kbps", kb,  sizeof(kb));
    uint32_t ip = (uint32_t)strtoul(ips, NULL, 10);
    if (ip && kb[0]) shaper_set_override(ip, atoi(kb));
    return redirect_to(req, "/admin", NULL);
}

static esp_err_t admin_logout_handler(httpd_req_t *req) {
    char tok[33];
    if (admin_cookie(req, tok))
        for (int s = 0; s < MAX_SESS; s++)
            if (strcmp(s_sess[s].tok, tok) == 0) s_sess[s].exp_us = 0;
    return redirect_to(req, "/admin",
                       "wt_admin=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
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
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;   // headroom for admin handlers (default 4096 too tight)

    httpd_handle_t server;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    // Admin routes must be registered before the catch-alls so they win the match.
    httpd_uri_t admin[] = {
        { .uri = "/admin",          .method = HTTP_GET,  .handler = admin_get_handler },
        { .uri = "/admin/setpw",    .method = HTTP_POST, .handler = admin_setpw_handler },
        { .uri = "/admin/login",    .method = HTTP_POST, .handler = admin_login_handler },
        { .uri = "/admin/settings", .method = HTTP_POST, .handler = admin_settings_handler },
        { .uri = "/admin/kick",     .method = HTTP_POST, .handler = admin_kick_handler },
        { .uri = "/admin/userspeed",.method = HTTP_POST, .handler = admin_userspeed_handler },
        { .uri = "/admin/logout",   .method = HTTP_POST, .handler = admin_logout_handler },
    };
    for (int i = 0; i < (int)(sizeof(admin) / sizeof(admin[0])); i++)
        httpd_register_uri_handler(server, &admin[i]);

    httpd_uri_t get_root  = { .uri = "/",  .method = HTTP_GET,  .handler = portal_get_handler };
    httpd_uri_t post_root = { .uri = "/",  .method = HTTP_POST, .handler = portal_post_handler };
    httpd_uri_t get_wild  = { .uri = "/*", .method = HTTP_GET,  .handler = portal_catchall_handler };
    httpd_uri_t post_wild = { .uri = "/*", .method = HTTP_POST, .handler = portal_catchall_handler };
    httpd_register_uri_handler(server, &get_root);
    httpd_register_uri_handler(server, &post_root);
    httpd_register_uri_handler(server, &get_wild);
    httpd_register_uri_handler(server, &post_wild);

    ESP_LOGI(TAG, "Captive portal HTTP server ready (admin at /admin)");
}
