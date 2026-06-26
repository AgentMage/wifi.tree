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

// 6-byte MAC <-> 12-char lowercase hex (no separators), for admin form values.
static void mac_to_hex(char out[13], const uint8_t mac[6]) {
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        out[i * 2]     = H[mac[i] >> 4];
        out[i * 2 + 1] = H[mac[i] & 0xF];
    }
    out[12] = '\0';
}

// Parse 12 hex chars into mac[6]. Returns false unless exactly 12 hex digits.
static bool hex_to_mac(const char *s, uint8_t mac[6]) {
    for (int i = 0; i < 12; i++)
        if (!isxdigit((unsigned char)s[i])) return false;
    if (s[12] != '\0') return false;
    for (int i = 0; i < 6; i++) {
        char b[3] = { s[i * 2], s[i * 2 + 1], 0 };
        mac[i] = (uint8_t)strtol(b, NULL, 16);
    }
    return true;
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

// Renders the "you've used your share" card for a visitor over their time budget.
static esp_err_t send_over_budget_card(httpd_req_t *req, client_t *c) {
    char safe_name[256];
    html_escape(safe_name, sizeof(safe_name), c->name[0] ? c->name : "friend");

    unsigned used_min = c->total_connected_s / 60;
    uint32_t mb = (uint32_t)(c->total_bytes >> 20);
    uint32_t tenths = (uint32_t)(((c->total_bytes & 0xFFFFF) * 10) >> 20);
    char body[768];
    int len = snprintf(body, sizeof(body),
        "<div class='card'>"
        "<p class='headline' style='color:#d8b24a'>Your leaf has fallen, %s &#x1F342;</p>"
        "<p class='sub'>You've used your share at this gathering "
        "(<strong>%u min</strong> online, <strong>%lu.%lu MB</strong>).</p>"
        "<p class='sub' style='opacity:.7'>Find the host to be let back on if you "
        "need more.</p>"
        "</div>",
        safe_name, used_min, (unsigned long)mb, (unsigned long)tenths);

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
    if (c && c->banned)
        return send_over_budget_card(req, c);
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
    if (c && c->banned)             // over their time budget — no new leaf
        return send_over_budget_card(req, c);
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

// Authed admin page: header + nav chrome around the body.
static esp_err_t send_admin(httpd_req_t *req, const char *body, int len) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, ADMIN_HEAD, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, body, len);
    httpd_resp_send_chunk(req, ADMIN_FOOT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// Unauthenticated admin page (login / set password): themed but no nav.
static esp_err_t send_admin_bare(httpd_req_t *req, const char *body, int len) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, ADMIN_HEAD_BARE, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, body, len);
    httpd_resp_send_chunk(req, ADMIN_FOOT, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t admin_dashboard(httpd_req_t *req) {
    int ttl = config_leaf_ttl_seconds();

    // Heap-allocate the large buffers — too big for the httpd task stack.
    const int BODY_SZ = 16384;
    client_t *list = malloc(sizeof(client_t) * 16);
    char *body = malloc(BODY_SZ);
    if (!list || !body) {
        free(list); free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    int n = clients_snapshot(list, 16);

    int cap  = config_connected_cap_seconds();
    int dcap = config_data_cap_mb();

    int o = snprintf(body, BODY_SZ,
        "<div class='cards'>"
        "<div class='stat'><div class='n' id='c-conn'>%d</div><div class='l'>connected now</div></div>"
        "<div class='stat'><div class='n'>%d</div><div class='l'>known visitors</div></div>"
        "<div class='stat'><div class='n' id='c-up'>%s</div><div class='l'>uplink</div></div>"
        "</div>"
        "<h2>Visitors</h2>",
        clients_count(), n, wifi_has_uplink() ? "online" : "&hellip;");

    for (int i = 0; i < n && o < BODY_SZ - 1100; i++) {
        char nm[128], hn[128], machex[13];
        html_escape(nm, sizeof(nm), list[i].name[0] ? list[i].name : "(no name yet)");
        html_escape(hn, sizeof(hn), list[i].hostname);
        mac_to_hex(machex, list[i].mac);
        int left = client_leaf_seconds_left(&list[i], ttl);

        // Status pill: bad=over budget, warn=no/expired leaf, ok=fresh/active.
        const char *pill = "ok";
        char status[48];
        if (list[i].banned)       { pill = "bad";  snprintf(status, sizeof(status), "over budget"); }
        else if (list[i].leaf_grown_us == 0)        { pill = "warn"; snprintf(status, sizeof(status), "no leaf"); }
        else if (!client_leaf_active(&list[i], ttl)){ pill = "warn"; snprintf(status, sizeof(status), "expired"); }
        else if (ttl <= 0)        { snprintf(status, sizeof(status), "fresh"); }
        else snprintf(status, sizeof(status), "%dh %dm left", left / 3600, (left % 3600) / 60);

        char budget[40];
        unsigned used_min = list[i].total_connected_s / 60;
        if (cap > 0) snprintf(budget, sizeof(budget), "%u / %d min", used_min, cap / 60);
        else         snprintf(budget, sizeof(budget), "%u min", used_min);

        // Lifetime data as "X.Y MB" (integer math — no float printf on this build).
        char data[40];
        uint32_t mb = (uint32_t)(list[i].total_bytes >> 20);
        uint32_t tenths = (uint32_t)(((list[i].total_bytes & 0xFFFFF) * 10) >> 20);
        if (dcap > 0) snprintf(data, sizeof(data), "%lu.%lu / %d MB",
                               (unsigned long)mb, (unsigned long)tenths, dcap);
        else          snprintf(data, sizeof(data), "%lu.%lu MB",
                               (unsigned long)mb, (unsigned long)tenths);

        // Per-user speed cap: -1 = global default, 0 = uncapped, else kbps.
        char capstr[20], capval[12];
        if (list[i].bw_cap_kbps < 0)       { snprintf(capstr, sizeof(capstr), "default"); capval[0] = '\0'; }
        else if (list[i].bw_cap_kbps == 0) { snprintf(capstr, sizeof(capstr), "uncapped"); snprintf(capval, sizeof(capval), "0"); }
        else { snprintf(capstr, sizeof(capstr), "%d kbps", (int)list[i].bw_cap_kbps);
               snprintf(capval, sizeof(capval), "%d", (int)list[i].bw_cap_kbps); }

        o += snprintf(body + o, BODY_SZ - o,
            "<div class='card2'>"
            "<div style='display:flex;justify-content:space-between;align-items:center;gap:8px'>"
            "<strong>&#x1F33F; %s</strong><span class='pill %s'>%s</span></div>"
            "<div class='muted' style='margin:5px 0 10px'>%s%sonline %s &middot; %s &middot; speed %s</div>"
            "<div style='display:flex;gap:6px;flex-wrap:wrap'>",
            nm, pill, status,
            hn[0] ? hn : "", hn[0] ? " &middot; " : "", budget, data, capstr);

        // Per-user speed cap — MAC-keyed (persists, works offline). Blank = default.
        o += snprintf(body + o, BODY_SZ - o,
            "<form method='POST' action='/admin/userspeed' "
            "style='display:flex;gap:6px;margin:0;flex:1;min-width:150px'>"
            "<input type='hidden' name='mac' value='%s'>"
            "<input type='number' name='kbps' min='0' value='%s' placeholder='default' "
            "style='margin:0;flex:1'>"
            "<button style='margin:0'>Cap</button></form>",
            machex, capval);

        if (list[i].ip) {  // kick acts on live traffic → needs the IP
            unsigned long ip = (unsigned long)list[i].ip;
            o += snprintf(body + o, BODY_SZ - o,
                "<form method='POST' action='/admin/kick' style='margin:0'>"
                "<input type='hidden' name='ip' value='%lu'>"
                "<button class='danger' style='margin:0'>Kick</button></form>",
                ip);
        }
        // Reset clears both budgets and lifts any ban. MAC-keyed (works offline).
        o += snprintf(body + o, BODY_SZ - o,
            "<form method='POST' action='/admin/resettime' style='margin:0'>"
            "<input type='hidden' name='mac' value='%s'>"
            "<button class='sec' style='margin:0'>Reset</button>"
            "</form></div></div>",
            machex);
    }
    if (n == 0)
        o += snprintf(body + o, BODY_SZ - o,
            "<p class='muted'>No visitors yet.</p>");

    if (o > BODY_SZ - 1) o = BODY_SZ - 1; // snprintf returns would-be length; clamp
    esp_err_t r = send_admin(req, body, o);
    free(list);
    free(body);
    return r;
}

// GET /admin/settings — the global settings form on its own themed page.
static esp_err_t admin_settings_get_handler(httpd_req_t *req) {
    if (!config_has_admin_password() || !admin_authed(req))
        return redirect_to(req, "/admin", NULL);
    char body[1400];
    int len = snprintf(body, sizeof(body),
        "<h2>Settings</h2>"
        "<div class='card2'>"
        "<form method='POST' action='/admin/settings'>"
        "<label class='muted'>Leaf freshness (minutes, 0 = never)</label><br>"
        "<input type='number' name='ttl_min' min='0' value='%d' style='width:120px'><br><br>"
        "<label class='muted'>Default speed per device (kbps, 0 = uncapped)</label><br>"
        "<input type='number' name='kbps' min='0' value='%d' style='width:120px'><br><br>"
        "<label class='muted'>Connected-time budget per visitor (minutes, 0 = unlimited)</label><br>"
        "<input type='number' name='tcap_min' min='0' value='%d' style='width:120px'><br><br>"
        "<label class='muted'>Data budget per visitor (MB, 0 = unlimited)</label><br>"
        "<input type='number' name='dcap_mb' min='0' value='%d' style='width:120px'><br><br>"
        "<button type='submit'>Save settings</button>"
        "</form></div>"
        "<p class='muted'>Speed caps each visitor up and down. The time and data "
        "budgets are lifetime totals &mdash; past either, a visitor is cut off "
        "until you Reset them on the dashboard.</p>",
        config_leaf_ttl_seconds() / 60, config_client_kbps(),
        config_connected_cap_seconds() / 60, config_data_cap_mb());
    return send_admin(req, body, len);
}

static esp_err_t admin_get_handler(httpd_req_t *req) {
    char body[700];
    if (!config_has_admin_password()) {
        int len = snprintf(body, sizeof(body),
            "<div class='login'>"
            "<h1>&#x1F333; wifi.tree</h1>"
            "<p class='muted' style='text-align:center'>set an admin password</p>"
            "<p class='muted'>Protects <strong>wifi.tree/admin</strong>. "
            "Set this before anyone joins.</p>"
            "<form method='POST' action='/admin/setpw'>"
            "<input type='password' name='pw' placeholder='new admin password' required>"
            "<button type='submit'>Save password</button>"
            "</form></div>");
        return send_admin_bare(req, body, len);
    }
    if (!admin_authed(req)) {
        int len = snprintf(body, sizeof(body),
            "<div class='login'>"
            "<h1>&#x1F333; wifi.tree</h1>"
            "<p class='muted' style='text-align:center'>admin panel</p>"
            "<form method='POST' action='/admin/login'>"
            "<input type='password' name='pw' placeholder='admin password' autofocus required>"
            "<button type='submit'>Log in</button>"
            "</form></div>");
        return send_admin_bare(req, body, len);
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
    char ttl[16] = {0}, kbps[16] = {0}, tcap[16] = {0}, dcap[16] = {0};
    get_field(rbody, "ttl_min",  ttl,  sizeof(ttl));
    get_field(rbody, "kbps",     kbps, sizeof(kbps));
    get_field(rbody, "tcap_min", tcap, sizeof(tcap));
    get_field(rbody, "dcap_mb",  dcap, sizeof(dcap));
    if (ttl[0])  config_set_leaf_ttl_seconds(atoi(ttl) * 60);
    if (kbps[0]) config_set_client_kbps(atoi(kbps));
    if (tcap[0]) config_set_connected_cap_seconds(atoi(tcap) * 60);
    if (dcap[0]) config_set_data_cap_mb(atoi(dcap));
    return redirect_to(req, "/admin/settings", NULL);
}

// Reset one visitor's connected-time budget (and lift any over-budget ban).
static esp_err_t admin_resettime_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char machex[16] = {0};
    get_field(rbody, "mac", machex, sizeof(machex));
    uint8_t mac[6];
    if (hex_to_mac(machex, mac)) {
        clients_reset_budget_by_mac(mac);
        clients_flush();
        ESP_LOGI(TAG, "admin reset time budget for %s", machex);
    }
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

// Set one visitor's persistent per-user speed cap (kbps; 0 = uncapped, blank =
// clear back to the global default). MAC-keyed so it survives reboot.
static esp_err_t admin_userspeed_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char machex[16] = {0}, kb[16] = {0};
    get_field(rbody, "mac",  machex, sizeof(machex));
    get_field(rbody, "kbps", kb,     sizeof(kb));
    uint8_t mac[6];
    if (hex_to_mac(machex, mac)) {
        int kbps = kb[0] ? atoi(kb) : -1;        // blank → use global default
        uint32_t ip = clients_set_bw_cap_by_mac(mac, kbps);
        if (ip) shaper_set_override(ip, kbps);   // apply now if they're online
        clients_flush();
        ESP_LOGI(TAG, "admin set %s speed cap to %d kbps", machex, kbps);
    }
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
        { .uri = "/admin/settings", .method = HTTP_GET,  .handler = admin_settings_get_handler },
        { .uri = "/admin/setpw",    .method = HTTP_POST, .handler = admin_setpw_handler },
        { .uri = "/admin/login",    .method = HTTP_POST, .handler = admin_login_handler },
        { .uri = "/admin/settings", .method = HTTP_POST, .handler = admin_settings_handler },
        { .uri = "/admin/kick",     .method = HTTP_POST, .handler = admin_kick_handler },
        { .uri = "/admin/userspeed",.method = HTTP_POST, .handler = admin_userspeed_handler },
        { .uri = "/admin/resettime",.method = HTTP_POST, .handler = admin_resettime_handler },
        { .uri = "/admin/logout",   .method = HTTP_GET,  .handler = admin_logout_handler },
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
