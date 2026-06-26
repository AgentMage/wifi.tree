#include "http_server.h"
#include "wifi_manager.h"
#include "client_state.h"
#include "config.h"
#include "portal_cfg.h"
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
#include "esp_wifi.h"

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
    char raw[768];
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
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

// Human "X ago" for an esp_timer timestamp (RAM-only; "&mdash;" if never seen).
static void fmt_ago(char *out, size_t sz, int64_t when_us) {
    if (when_us <= 0) { strlcpy(out, "&mdash;", sz); return; }
    int s = (int)((esp_timer_get_time() - when_us) / 1000000);
    if (s < 0) s = 0;
    if (s < 60)        snprintf(out, sz, "%ds ago", s);
    else if (s < 3600) snprintf(out, sz, "%dm ago", s / 60);
    else               snprintf(out, sz, "%dh ago", s / 3600);
}

// Escapes a string for embedding inside a JSON double-quoted value. Leaves
// HTML metachars raw (the browser HTML-escapes on render); handles ", \, control.
static void json_escape(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    for (; *src && i + 7 < dstsz; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[i++] = '\\'; dst[i++] = c; }
        else if (c == '\n') { dst[i++] = '\\'; dst[i++] = 'n'; }
        else if (c < 0x20)  { i += snprintf(dst + i, dstsz - i, "\\u%04x", c); }
        else dst[i++] = c;
    }
    dst[i] = '\0';
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

// Copy only safe hex-colour characters; fall back to the default accent.
static void accent_safe(char *out, size_t sz, const char *in) {
    size_t j = 0;
    for (; *in && j + 1 < sz; in++) {
        char c = *in;
        if (c == '#' || (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            out[j++] = c;
    }
    out[j] = '\0';
    if (!out[0]) strlcpy(out, "#2e7d32", sz);
}

// Like html_escape, but turns newlines into <br> (for multi-line copy fields).
static void escape_br(char *dst, size_t dstsz, const char *src) {
    size_t i = 0;
    for (; *src && i + 7 < dstsz; src++) {
        switch (*src) {
            case '<':  memcpy(dst+i, "&lt;",  4); i += 4; break;
            case '>':  memcpy(dst+i, "&gt;",  4); i += 4; break;
            case '&':  memcpy(dst+i, "&amp;", 5); i += 5; break;
            case '\n': memcpy(dst+i, "<br>",  4); i += 4; break;
            default:   dst[i++] = *src; break;
        }
    }
    dst[i] = '\0';
}

// Emit the customizable portal page head (theme + logo/title/tagline + banner).
static esp_err_t send_portal_head(httpd_req_t *req) {
    char title[120], emoji[64], tag[400], accent[10];
    html_escape(title, sizeof(title), portalcfg_get("title"));
    html_escape(emoji, sizeof(emoji), portalcfg_get("emoji"));
    html_escape(tag,   sizeof(tag),   portalcfg_get("tagline"));
    accent_safe(accent, sizeof(accent), portalcfg_get("accent"));

    char pre[256];
    int n = snprintf(pre, sizeof(pre),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title><style>", title);
    if (n >= (int)sizeof(pre)) n = sizeof(pre) - 1;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, pre, n);
    httpd_resp_send_chunk(req, PORTAL_CSS_STR, sizeof(PORTAL_CSS_STR) - 1);

    char post[700];
    n = snprintf(post, sizeof(post),
        "button,a.btnlink{background:%s}</style></head><body><div class='wrap'>"
        "<div class='logo'>%s</div><h1>%s</h1><p class='tag'>%s</p>",
        accent, emoji, title, tag);
    if (n >= (int)sizeof(post)) n = sizeof(post) - 1;
    httpd_resp_send_chunk(req, post, n);

    const char *banner = portalcfg_get("banner");
    if (banner[0]) {
        char be[640], bbuf[760];
        escape_br(be, sizeof(be), banner);
        n = snprintf(bbuf, sizeof(bbuf),
            "<div class='card' style='border-color:#b8860b;background:#2a2410;"
            "color:#ffe9a8;white-space:pre-wrap'>%s</div>", be);
        if (n >= (int)sizeof(bbuf)) n = sizeof(bbuf) - 1;
        httpd_resp_send_chunk(req, bbuf, n);
    }
    return ESP_OK;
}

// Emit the customizable portal page footer + closing tags. Ends the response.
static esp_err_t send_portal_foot(httpd_req_t *req) {
    char fe[640], buf[760];
    escape_br(fe, sizeof(fe), portalcfg_get("footer"));
    int n = snprintf(buf, sizeof(buf), "<p class='foot'>%s</p></div></body></html>", fe);
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    httpd_resp_send_chunk(req, buf, n);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
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

    send_portal_head(req);
    httpd_resp_send_chunk(req, body, len);
    return send_portal_foot(req);
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

    send_portal_head(req);
    httpd_resp_send_chunk(req, body, len);
    return send_portal_foot(req);
}

static esp_err_t portal_get_handler(httpd_req_t *req) {
    int ttl = config_leaf_ttl_seconds();
    uint32_t ip = client_ip(req);
    client_t *c = clients_find_by_ip(ip);
    if (c && c->banned)
        return send_over_budget_card(req, c);
    if (client_leaf_active(c, ttl)) {
        // Re-open the gate for this IP — covers a reconnect that got a new DHCP
        // lease (leaf is keyed by MAC, but the internet grant is keyed by IP).
        if (ip) {
            int64_t expiry = ttl <= 0 ? 0 : c->leaf_grown_us + (int64_t)ttl * 1000000;
            authz_grant(ip, expiry);
        }
        return send_status_card(req, c, client_leaf_seconds_left(c, ttl));
    }

    // No fresh leaf. A returning visitor (we have their name) gets a personalized
    // "welcome back / renew" card with the name pre-filled; a brand-new visitor
    // gets the customizable welcome + empty name form.
    char accent[10];
    accent_safe(accent, sizeof(accent), portalcfg_get("accent"));

    char headline[320], subline[768], nameval[256];
    const char *btn;
    if (c && c->name[0]) {                       // known, leaf wilted → renew
        char nm[256];
        html_escape(nm, sizeof(nm), c->name);
        snprintf(headline, sizeof(headline), "Welcome back, %s &#x1F33F;", nm);
        strlcpy(subline, "Your leaf has wilted. Grow a fresh one to get back online.",
                sizeof(subline));
        strlcpy(nameval, nm, sizeof(nameval));
        btn = "Renew my leaf &#x1F33F;";
    } else {                                     // brand-new visitor
        html_escape(headline, sizeof(headline), portalcfg_get("whead"));
        escape_br(subline, sizeof(subline), portalcfg_get("wtext"));
        nameval[0] = '\0';
        btn = "Grow a Leaf &#x1F33F;";
    }

    char body[1600];
    int len = snprintf(body, sizeof(body),
        "<div class='card'>"
        "<p class='headline' style='color:%s'>%s</p>"
        "<p class='sub' style='white-space:pre-wrap'>%s</p>"
        "</div>"
        "<div class='card'>"
        "<form method='POST'>"
        "<input name='name' value='%s' placeholder='enter your name' maxlength='40' required>"
        "<button type='submit'>%s</button>"
        "</form></div>",
        accent, headline, subline, nameval, btn);
    if (len >= (int)sizeof(body)) len = sizeof(body) - 1;

    send_portal_head(req);
    httpd_resp_send_chunk(req, body, len);
    return send_portal_foot(req);
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

    char page[700];
    int len = snprintf(page, sizeof(page),
        "<div class='card ok leafy'><div class='success'>"
        "<div class='leaf-burst'><span class='sway'>&#x1F33F;</span></div>"
        "<p class='leaf-sub'>a fresh leaf, just for you</p>"
        "<div class='big'>You grew a leaf, %s!</div>"
        "<p class='note'>You&#39;re online &mdash; close this page and start browsing.</p>"
        "</div>"
        "<a class='btnlink' href='http://wifi.tree'>Go to wifi.tree &rarr;</a>"
        "</div>",
        safe_name);

    send_portal_head(req);
    httpd_resp_send_chunk(req, page, len);
    return send_portal_foot(req);
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

// Which list a visitor belongs in: 0 = active now (fresh leaf), 1 = registered
// (has a name but no active leaf), 2 = not yet registered (no name).
static int visitor_group(const client_t *u, int ttl) {
    if (client_leaf_active(u, ttl)) return 0;
    if (u->name[0]) return 1;
    return 2;
}

static esp_err_t admin_dashboard(httpd_req_t *req) {
    int ttl = config_leaf_ttl_seconds();

    // Heap-allocate the large buffers — too big for the httpd task stack.
    const int BODY_SZ = 36864;
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

    uint8_t pri = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&pri, &sec);

    int o = snprintf(body, BODY_SZ,
        "<div class='cards'>"
        "<div class='stat'><div class='n' id='c-conn'>%d</div><div class='l'>connected now</div></div>"
        "<div class='stat'><div class='n' id='c-down'>&ndash;</div><div class='l'>download</div></div>"
        "<div class='stat'><div class='n' id='c-up'>&ndash;</div><div class='l'>upload</div></div>"
        "<div class='stat'><div class='n' id='c-ch'>%d</div><div class='l'>channel</div></div>"
        "</div>"
        "<p class='muted' style='margin-top:-10px'>uplink: %s &middot; %d known visitor(s)</p>"
        "<h2>Live traffic</h2>"
        "<table><thead><tr><th>device</th><th>name</th><th>signal</th>"
        "<th>down</th><th>up</th><th>total</th></tr></thead>"
        "<tbody id='live'><tr><td colspan='6' class='muted'>loading&hellip;</td></tr></tbody></table>",
        clients_count(), pri, wifi_has_uplink() ? "online" : "connecting&hellip;", n);

    static const char *HEAD[3] = { "Active now", "Registered", "Not yet registered" };
    for (int g = 0; g < 3 && o < BODY_SZ - 2400; g++) {
        int members = 0;
        for (int gi = 0; gi < n; gi++) if (visitor_group(&list[gi], ttl) == g) members++;
        o += snprintf(body + o, BODY_SZ - o, "<h2>%s (%d)</h2>", HEAD[g], members);
        if (!members) {
            o += snprintf(body + o, BODY_SZ - o, "<p class='muted'>None.</p>");
            continue;
        }
        for (int i = 0; i < n && o < BODY_SZ - 2400; i++) {
            if (visitor_group(&list[i], ttl) != g) continue;
            client_t *u = &list[i];
            char nm[128], rawnm[128], hn[128], machex[13], macfmt[20], ago[24];
        html_escape(nm, sizeof(nm), u->name[0] ? u->name : "(no name yet)");
        html_escape(rawnm, sizeof(rawnm), u->name);   // for the rename input
        html_escape(hn, sizeof(hn), u->hostname);
        mac_to_hex(machex, u->mac);
        snprintf(macfmt, sizeof(macfmt), "%02x:%02x:%02x:%02x:%02x:%02x",
                 u->mac[0], u->mac[1], u->mac[2], u->mac[3], u->mac[4], u->mac[5]);
        fmt_ago(ago, sizeof(ago), u->last_seen_us);
        int left = client_leaf_seconds_left(u, ttl);
        bool exempt = (u->tcap_override == 0 && u->dcap_override == 0);

        // Status pill: bad=over budget, warn=no/expired leaf, ok=fresh/active.
        const char *pill = "ok";
        char status[48];
        if (u->banned)            { pill = "bad";  snprintf(status, sizeof(status), "over budget"); }
        else if (u->leaf_grown_us == 0)        { pill = "warn"; snprintf(status, sizeof(status), "no leaf"); }
        else if (!client_leaf_active(u, ttl))  { pill = "warn"; snprintf(status, sizeof(status), "expired"); }
        else if (ttl <= 0)        { snprintf(status, sizeof(status), "fresh"); }
        else snprintf(status, sizeof(status), "%dh %dm left", left / 3600, (left % 3600) / 60);

        char ipstr[20];
        if (u->ip) {
            const uint8_t *b = (const uint8_t *)&u->ip;
            snprintf(ipstr, sizeof(ipstr), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        } else snprintf(ipstr, sizeof(ipstr), "offline");

        // Effective budgets: per-visitor override (>=0) wins, else global; 0 = unlimited.
        unsigned used_min = u->total_connected_s / 60;
        int eff_tmin = u->tcap_override >= 0 ? u->tcap_override / 60 : (cap > 0 ? cap / 60 : 0);
        uint32_t mb = (uint32_t)(u->total_bytes >> 20);
        uint32_t tenths = (uint32_t)(((u->total_bytes & 0xFFFFF) * 10) >> 20);
        uint32_t used_tenths = (uint32_t)((u->total_bytes * 10) >> 20);
        int eff_dmb = u->dcap_override >= 0 ? u->dcap_override : (dcap > 0 ? dcap : 0);

        // Build optional progress bars (only when a finite cap applies).
        char tbar[170] = "", dbar[170] = "", ttxt[48], dtxt[48];
        if (eff_tmin > 0) {
            int pct = (int)(used_min * 100 / (unsigned)eff_tmin); if (pct > 100) pct = 100;
            const char *col = pct < 75 ? "#2e7d32" : (pct < 100 ? "#b8860b" : "#a33");
            snprintf(tbar, sizeof(tbar),
                "<span class='bar'><span class='f' style='width:%d%%;background:%s'></span></span>", pct, col);
            snprintf(ttxt, sizeof(ttxt), "%u/%d min", used_min, eff_tmin);
        } else snprintf(ttxt, sizeof(ttxt), "%u min", used_min);
        if (eff_dmb > 0) {
            int pct = (int)(used_tenths * 10 / (uint32_t)eff_dmb); if (pct > 100) pct = 100;
            const char *col = pct < 75 ? "#2e7d32" : (pct < 100 ? "#b8860b" : "#a33");
            snprintf(dbar, sizeof(dbar),
                "<span class='bar'><span class='f' style='width:%d%%;background:%s'></span></span>", pct, col);
            snprintf(dtxt, sizeof(dtxt), "%lu.%lu/%d MB", (unsigned long)mb, (unsigned long)tenths, eff_dmb);
        } else snprintf(dtxt, sizeof(dtxt), "%lu.%lu MB", (unsigned long)mb, (unsigned long)tenths);

        // Per-user override prefills (blank = use global default).
        char capval[12], tval[12], dval[12], capstr[20];
        if (u->bw_cap_kbps < 0)       { capval[0] = '\0'; snprintf(capstr, sizeof(capstr), "default"); }
        else if (u->bw_cap_kbps == 0) { snprintf(capval, sizeof(capval), "0"); snprintf(capstr, sizeof(capstr), "uncapped"); }
        else { snprintf(capval, sizeof(capval), "%d", (int)u->bw_cap_kbps);
               snprintf(capstr, sizeof(capstr), "%d kbps", (int)u->bw_cap_kbps); }
        if (u->tcap_override < 0) tval[0] = '\0'; else snprintf(tval, sizeof(tval), "%d", (int)u->tcap_override / 60);
        if (u->dcap_override < 0) dval[0] = '\0'; else snprintf(dval, sizeof(dval), "%d", (int)u->dcap_override);

        // Card head: name + status, two meta lines, usage bars + speed chip.
        o += snprintf(body + o, BODY_SZ - o,
            "<div class='card2'>"
            "<div class='vhead'><span class='vname'>&#x1F33F; %s</span>"
            "<span class='pill %s'>%s</span></div>"
            "<div class='vmeta'>%s%s%s</div>"
            "<div class='vmeta' style='margin-top:-4px'>%s &middot; seen %s</div>"
            "<div class='bcell'>&#x23F1;&#xFE0F; %s<span class='bnum'>%s</span></div>"
            "<div class='bcell'>&#x1F4CA; %s<span class='bnum'>%s</span></div>"
            "<div class='bcell'>&#x1F6A6; <span class='bnum'>%s</span>%s</div>"
            "<details class='manage'><summary>Manage</summary>",
            nm, pill, status,
            hn[0] ? hn : "", hn[0] ? " &middot; " : "", ipstr,
            macfmt, ago,
            tbar, ttxt, dbar, dtxt, capstr,
            exempt ? " <span class='pill ok'>exempt</span>" : "");

        // Manage: rename, speed, time, data overrides.
        o += snprintf(body + o, BODY_SZ - o,
            "<div class='mrow'><form method='POST' action='/admin/rename'>"
            "<input type='hidden' name='mac' value='%s'><label>Name</label>"
            "<input type='text' name='name' value='%s' placeholder='(no name)' maxlength='40'>"
            "<button class='sec'>Rename</button></form></div>"
            "<div class='mrow'><form method='POST' action='/admin/userspeed'>"
            "<input type='hidden' name='mac' value='%s'><label>Speed</label>"
            "<input type='number' name='kbps' min='0' value='%s' placeholder='default'>"
            "<span class='muted'>kbps</span><button class='sec'>Set</button></form></div>"
            "<div class='mrow'><form method='POST' action='/admin/usertime'>"
            "<input type='hidden' name='mac' value='%s'><label>Time</label>"
            "<input type='number' name='min' min='0' value='%s' placeholder='default'>"
            "<span class='muted'>min</span><button class='sec'>Set</button></form></div>"
            "<div class='mrow'><form method='POST' action='/admin/userdata'>"
            "<input type='hidden' name='mac' value='%s'><label>Data</label>"
            "<input type='number' name='mb' min='0' value='%s' placeholder='default'>"
            "<span class='muted'>MB</span><button class='sec'>Set</button></form></div>"
            "<p class='muted' style='font-size:.78em;margin:4px 0 0'>"
            "Blank = use the global default &middot; 0 = unlimited.</p>",
            machex, rawnm, machex, capval, machex, tval, machex, dval);

        // Manage actions: exempt toggle, reset, kick, forget.
        o += snprintf(body + o, BODY_SZ - o,
            "<div class='mactions'>"
            "<form method='POST' action='/admin/exempt'>"
            "<input type='hidden' name='mac' value='%s'>"
            "<input type='hidden' name='on' value='%s'>"
            "<button class='sec'>%s</button></form>"
            "<form method='POST' action='/admin/resettime'>"
            "<input type='hidden' name='mac' value='%s'>"
            "<button class='sec'>Reset usage</button></form>",
            machex, exempt ? "0" : "1",
            exempt ? "Un-exempt" : "Exempt (unlimited)", machex);

        if (u->ip)   // kick acts on live traffic → needs the IP
            o += snprintf(body + o, BODY_SZ - o,
                "<form method='POST' action='/admin/kick' "
                "onsubmit='return confirm(\"Kick this visitor now?\")'>"
                "<input type='hidden' name='ip' value='%lu'>"
                "<button class='warn'>Kick</button></form>",
                (unsigned long)u->ip);

        o += snprintf(body + o, BODY_SZ - o,
            "<form method='POST' action='/admin/forget' "
            "onsubmit='return confirm(\"Forget this visitor entirely?\")'>"
            "<input type='hidden' name='mac' value='%s'>"
            "<button class='danger'>Forget</button></form>"
            "</div></details></div>",
            machex);
        }
    }

    o += snprintf(body + o, BODY_SZ - o, "%s", ADMIN_DASH_JS);

    if (o > BODY_SZ - 1) o = BODY_SZ - 1; // snprintf returns would-be length; clamp
    esp_err_t r = send_admin(req, body, o);
    free(list);
    free(body);
    return r;
}

// GET /admin/settings — global settings with on/off toggles + reset.
static esp_err_t admin_settings_get_handler(httpd_req_t *req) {
    if (!config_has_admin_password() || !admin_authed(req))
        return redirect_to(req, "/admin", NULL);

    int ttl = config_leaf_ttl_seconds(), kbps = config_client_kbps();
    int tcap = config_connected_cap_seconds(), dcap = config_data_cap_mb();
    // When a knob is off (0), pre-fill a sensible value for when it's re-enabled.
    int ttl_v  = ttl  > 0 ? ttl / 60   : 180;
    int kbps_v = kbps > 0 ? kbps       : 100;
    int tcap_v = tcap > 0 ? tcap / 60  : 60;
    int dcap_v = dcap > 0 ? dcap       : 100;
    const char *ttl_ck  = ttl  > 0 ? "checked" : "", *ttl_dis  = ttl  > 0 ? "" : "disabled";
    const char *kbps_ck = kbps > 0 ? "checked" : "", *kbps_dis = kbps > 0 ? "" : "disabled";
    const char *tcap_ck = tcap > 0 ? "checked" : "", *tcap_dis = tcap > 0 ? "" : "disabled";
    const char *dcap_ck = dcap > 0 ? "checked" : "", *dcap_dis = dcap > 0 ? "" : "disabled";

    char body[2200];
    int len = snprintf(body, sizeof(body),
        "<h2>Settings</h2>"
        "<div class='card2'><form class='cust' method='POST' action='/admin/settings'>"
        "<label>Leaf freshness</label>"
        "<div><label><input type='checkbox' name='leaf_on' %s onchange=\"t(this,'i_ttl')\"> "
        "leaves expire</label> "
        "<input type='number' id='i_ttl' name='ttl_min' min='1' value='%d' %s style='width:90px'> min</div>"
        "<label>Default speed per device</label>"
        "<div><label><input type='checkbox' name='kbps_on' %s onchange=\"t(this,'i_kbps')\"> "
        "cap speed</label> "
        "<input type='number' id='i_kbps' name='kbps' min='1' value='%d' %s style='width:90px'> kbps</div>"
        "<label>Connected-time budget per visitor</label>"
        "<div><label><input type='checkbox' name='tcap_on' %s onchange=\"t(this,'i_tcap')\"> "
        "limit time</label> "
        "<input type='number' id='i_tcap' name='tcap_min' min='1' value='%d' %s style='width:90px'> min</div>"
        "<label>Data budget per visitor</label>"
        "<div><label><input type='checkbox' name='dcap_on' %s onchange=\"t(this,'i_dcap')\"> "
        "limit data</label> "
        "<input type='number' id='i_dcap' name='dcap_mb' min='1' value='%d' %s style='width:90px'> MB</div>"
        "<div style='margin-top:18px'><button type='submit'>Save settings</button></div>"
        "</form></div>"
        "<form method='POST' action='/admin/settings' style='margin-top:6px' "
        "onsubmit='return confirm(\"Reset settings to defaults?\")'>"
        "<input type='hidden' name='action' value='reset'>"
        "<button class='sec'>Reset to defaults</button></form>"
        "<p class='muted'>Unchecked = off (leaves never expire / speed uncapped / "
        "budget unlimited). Time and data budgets are lifetime totals.</p>"
        "<script>function t(cb,id){document.getElementById(id).disabled=!cb.checked}</script>",
        ttl_ck, ttl_v, ttl_dis, kbps_ck, kbps_v, kbps_dis,
        tcap_ck, tcap_v, tcap_dis, dcap_ck, dcap_v, dcap_dis);
    return send_admin(req, body, len);
}

// GET /admin/customize — portal appearance editor with a live preview.
static esp_err_t admin_customize_get_handler(httpd_req_t *req) {
    if (!config_has_admin_password() || !admin_authed(req))
        return redirect_to(req, "/admin", NULL);

    char emoji[64], title[120], tag[400], banner[640];
    char whead[200], wtext[760], footer[640], accent[10];
    html_escape(emoji,  sizeof(emoji),  portalcfg_get("emoji"));
    html_escape(title,  sizeof(title),  portalcfg_get("title"));
    html_escape(tag,    sizeof(tag),    portalcfg_get("tagline"));
    html_escape(banner, sizeof(banner), portalcfg_get("banner"));
    html_escape(whead,  sizeof(whead),  portalcfg_get("whead"));
    html_escape(wtext,  sizeof(wtext),  portalcfg_get("wtext"));
    html_escape(footer, sizeof(footer), portalcfg_get("footer"));
    accent_safe(accent, sizeof(accent), portalcfg_get("accent"));

    const int SZ = 8192;
    char *body = malloc(SZ);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    int o = snprintf(body, SZ,
        "<h2>Customize the portal</h2>"
        "<div class='preview'>"
        "<div class='pe' id='p-emoji'>%s</div>"
        "<div class='pt' id='p-title' style='color:%s'>%s</div>"
        "<div class='pg' id='p-tag'>%s</div>"
        "<div class='pb' id='p-banner' style='%s'>%s</div>"
        "</div>"
        "<div class='card2'><form class='cust' method='POST' action='/admin/customize'>"
        "<label>Header emoji</label>"
        "<input type='text' name='emoji' id='f-emoji' value='%s' maxlength='16' oninput='upd()'>"
        "<div class='emoji-pick'>" EMOJI_PICK "</div>"
        "<label>Title</label>"
        "<input type='text' name='title' id='f-title' value='%s' maxlength='40' oninput='upd()'>"
        "<label>Tagline</label>"
        "<input type='text' name='tagline' id='f-tag' value='%s' maxlength='120' oninput='upd()'>"
        "<label>Announcement banner (blank = hidden)</label>"
        "<textarea name='banner' id='f-banner' maxlength='200' oninput='upd()'>%s</textarea>"
        "<label>Welcome heading</label>"
        "<input type='text' name='whead' value='%s' maxlength='80'>"
        "<label>Welcome text</label>"
        "<textarea name='wtext' maxlength='400'>%s</textarea>"
        "<label>Footer</label>"
        "<textarea name='footer' maxlength='200'>%s</textarea>"
        "<label>Accent colour (buttons &amp; title)</label><br>"
        "<input type='color' name='accent' id='f-accent' value='%s' oninput='upd()'>"
        "<div style='margin-top:18px'><button type='submit'>Save &mdash; go live</button></div>"
        "</form></div>"
        "<form method='POST' action='/admin/customize' style='margin-top:6px' "
        "onsubmit='return confirm(\"Reset portal text to defaults?\")'>"
        "<input type='hidden' name='action' value='reset'>"
        "<button class='sec'>Reset to defaults</button></form>"
        "<p class='muted'>Changes go live on the portal immediately.</p>"
        CUSTOMIZE_JS,
        emoji, accent, title, tag, banner[0] ? "" : "display:none", banner,
        emoji, title, tag, banner, whead, wtext, footer, accent);

    if (o > SZ - 1) o = SZ - 1;
    esp_err_t r = send_admin(req, body, o);
    free(body);
    return r;
}

// POST /admin/customize — save appearance fields, or reset to defaults.
static esp_err_t admin_customize_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);

    const int SZ = 4096;
    char *rbody = malloc(SZ);
    if (!rbody) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int total = 0, remaining = req->content_len;
    if (remaining > SZ - 1) remaining = SZ - 1;
    while (remaining > 0) {
        int r = httpd_req_recv(req, rbody + total, remaining);
        if (r <= 0) break;
        total += r; remaining -= r;
    }
    rbody[total] = '\0';

    char act[16] = {0};
    get_field(rbody, "action", act, sizeof(act));
    if (strcmp(act, "reset") == 0) {
        portalcfg_reset();
        ESP_LOGI(TAG, "portal customization reset to defaults");
    } else {
        char val[420];
        for (int i = 0; i < portalcfg_count(); i++) {
            const portalcfg_field_t *f = portalcfg_field(i);
            get_field(rbody, f->key, val, sizeof(val));
            portalcfg_set(f->key, val);
        }
        ESP_LOGI(TAG, "portal customization saved");
    }
    free(rbody);
    return redirect_to(req, "/admin/customize", NULL);
}

// GET /admin/stats.json — live data for the dashboard's 2s poll.
static esp_err_t admin_stats_json(httpd_req_t *req) {
    if (!config_has_admin_password() || !admin_authed(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{}", 2);
        return ESP_OK;
    }
    const int SZ = 4096;
    client_view_t *v = malloc(sizeof(client_view_t) * 16);
    char *buf = malloc(SZ);
    if (!v || !buf) {
        free(v); free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int n = clients_live_view(v, 16);

    uint8_t pri = 0;
    wifi_second_chan_t sec;
    esp_wifi_get_channel(&pri, &sec);

    int o = snprintf(buf, SZ, "{\"conn\":%d,\"ch\":%d,\"up\":%d,\"clients\":[",
                     clients_count(), pri, wifi_has_uplink() ? 1 : 0);
    for (int i = 0; i < n && o < SZ - 320; i++) {
        const uint8_t *b = (const uint8_t *)&v[i].ip;
        char nm[120], hn[100];
        json_escape(nm, sizeof(nm), v[i].name);
        json_escape(hn, sizeof(hn), v[i].hostname);
        o += snprintf(buf + o, SZ - o,
            "%s{\"ip\":\"%u.%u.%u.%u\",\"name\":\"%s\",\"host\":\"%s\",\"rssi\":%d,"
            "\"dn\":%lu,\"up\":%lu,\"min\":%lu}",
            i ? "," : "", b[0], b[1], b[2], b[3], nm, hn, v[i].rssi,
            (unsigned long)(v[i].down >> 10), (unsigned long)(v[i].up >> 10),
            (unsigned long)(v[i].total_connected_s / 60));
    }
    o += snprintf(buf + o, SZ - o, "]}");
    if (o > SZ - 1) o = SZ - 1;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, o);
    free(v); free(buf);
    return ESP_OK;
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

    char act[16] = {0};
    get_field(rbody, "action", act, sizeof(act));
    if (strcmp(act, "reset") == 0) {              // factory defaults
        config_set_leaf_ttl_seconds(3 * 3600);
        config_set_client_kbps(100);
        config_set_connected_cap_seconds(0);
        config_set_data_cap_mb(0);
        return redirect_to(req, "/admin/settings", NULL);
    }

    // A checkbox absent from the POST (its number input is disabled, so the
    // browser omits both) means that knob is turned off → store 0.
    char on[8], v[16];
    get_field(rbody, "leaf_on", on, sizeof(on));
    get_field(rbody, "ttl_min", v, sizeof(v));
    config_set_leaf_ttl_seconds(on[0] ? atoi(v) * 60 : 0);
    get_field(rbody, "kbps_on", on, sizeof(on));
    get_field(rbody, "kbps", v, sizeof(v));
    config_set_client_kbps(on[0] ? atoi(v) : 0);
    get_field(rbody, "tcap_on", on, sizeof(on));
    get_field(rbody, "tcap_min", v, sizeof(v));
    config_set_connected_cap_seconds(on[0] ? atoi(v) * 60 : 0);
    get_field(rbody, "dcap_on", on, sizeof(on));
    get_field(rbody, "dcap_mb", v, sizeof(v));
    config_set_data_cap_mb(on[0] ? atoi(v) : 0);
    return redirect_to(req, "/admin/settings", NULL);
}

// POST /admin/rename — rename a visitor (MAC-keyed).
static esp_err_t admin_rename_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[256] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char machex[16] = {0}, name[64] = {0};
    get_field(rbody, "mac", machex, sizeof(machex));
    get_field(rbody, "name", name, sizeof(name));
    name[40] = '\0';
    uint8_t mac[6];
    if (hex_to_mac(machex, mac)) {
        clients_set_name_by_mac(mac, name);
        clients_flush();
    }
    return redirect_to(req, "/admin", NULL);
}

// POST /admin/forget — drop a visitor's record entirely and cut them off.
static esp_err_t admin_forget_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char machex[16] = {0};
    get_field(rbody, "mac", machex, sizeof(machex));
    uint8_t mac[6];
    if (hex_to_mac(machex, mac)) {
        uint32_t ip = clients_remove_by_mac(mac);
        if (ip) authz_revoke(ip);
        clients_flush();
        ESP_LOGI(TAG, "admin forgot %s", machex);
    }
    return redirect_to(req, "/admin", NULL);
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

// Per-visitor time budget override (minutes; blank = global, 0 = unlimited).
static esp_err_t admin_usertime_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char machex[16] = {0}, v[16] = {0};
    get_field(rbody, "mac", machex, sizeof(machex));
    get_field(rbody, "min", v, sizeof(v));
    uint8_t mac[6];
    if (hex_to_mac(machex, mac)) {
        clients_set_time_cap_by_mac(mac, v[0] ? atoi(v) * 60 : -1);
        clients_flush();
    }
    return redirect_to(req, "/admin", NULL);
}

// Per-visitor data budget override (MB; blank = global, 0 = unlimited).
static esp_err_t admin_userdata_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char machex[16] = {0}, v[16] = {0};
    get_field(rbody, "mac", machex, sizeof(machex));
    get_field(rbody, "mb", v, sizeof(v));
    uint8_t mac[6];
    if (hex_to_mac(machex, mac)) {
        clients_set_data_cap_by_mac(mac, v[0] ? atoi(v) : -1);
        clients_flush();
    }
    return redirect_to(req, "/admin", NULL);
}

// Exempt / un-exempt a visitor (lift all caps, or revert to global defaults).
static esp_err_t admin_exempt_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[128] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char machex[16] = {0}, on[8] = {0};
    get_field(rbody, "mac", machex, sizeof(machex));
    get_field(rbody, "on", on, sizeof(on));
    uint8_t mac[6];
    if (hex_to_mac(machex, mac)) {
        clients_set_exempt_by_mac(mac, on[0] == '1');
        clients_flush();
    }
    return redirect_to(req, "/admin", NULL);
}

// Render the change-password page, optionally with a status message.
static esp_err_t send_password_page(httpd_req_t *req, const char *msg, bool err) {
    char body[1000];
    int len = snprintf(body, sizeof(body),
        "<h2>Change admin password</h2>"
        "%s%s%s"
        "<div class='card2'><form method='POST' action='/admin/password'>"
        "<input type='password' name='current' placeholder='current password' "
        "required style='width:100%%;max-width:320px'><br><br>"
        "<input type='password' name='new1' placeholder='new password (min 8)' "
        "required style='width:100%%;max-width:320px'><br><br>"
        "<input type='password' name='new2' placeholder='repeat new password' "
        "required style='width:100%%;max-width:320px'><br><br>"
        "<button type='submit'>Change password</button>"
        "</form></div>"
        "<p class='muted'>Changing it signs out other devices.</p>",
        msg[0] ? (err ? "<p style='color:#ff8a80'>" : "<p style='color:#8ef08e'>") : "",
        msg, msg[0] ? "</p>" : "");
    return send_admin(req, body, len);
}

static esp_err_t admin_password_get_handler(httpd_req_t *req) {
    if (!config_has_admin_password() || !admin_authed(req))
        return redirect_to(req, "/admin", NULL);
    return send_password_page(req, "", false);
}

static esp_err_t admin_password_handler(httpd_req_t *req) {
    if (!admin_authed(req)) return redirect_to(req, "/admin", NULL);
    char rbody[256] = {0};
    int n = httpd_req_recv(req, rbody, sizeof(rbody) - 1);
    if (n > 0) rbody[n] = '\0';
    char cur[64] = {0}, n1[64] = {0}, n2[64] = {0};
    get_field(rbody, "current", cur, sizeof(cur));
    get_field(rbody, "new1", n1, sizeof(n1));
    get_field(rbody, "new2", n2, sizeof(n2));

    if (!config_check_admin_password(cur))
        return send_password_page(req, "Current password is wrong.", true);
    if (strlen(n1) < 8)
        return send_password_page(req, "New password must be at least 8 characters.", true);
    if (strcmp(n1, n2) != 0)
        return send_password_page(req, "New passwords don't match.", true);

    config_set_admin_password(n1);
    // Sign out every other session (keep the one making the change).
    char tok[33];
    bool have = admin_cookie(req, tok);
    for (int s = 0; s < MAX_SESS; s++)
        if (!have || strcmp(s_sess[s].tok, tok) != 0) s_sess[s].exp_us = 0;
    ESP_LOGI(TAG, "admin password changed");
    return send_password_page(req, "Password changed. Other devices signed out.", false);
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
    cfg.max_uri_handlers = 28;
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
        { .uri = "/admin/customize",.method = HTTP_GET,  .handler = admin_customize_get_handler },
        { .uri = "/admin/customize",.method = HTTP_POST, .handler = admin_customize_handler },
        { .uri = "/admin/stats.json",.method = HTTP_GET, .handler = admin_stats_json },
        { .uri = "/admin/setpw",    .method = HTTP_POST, .handler = admin_setpw_handler },
        { .uri = "/admin/login",    .method = HTTP_POST, .handler = admin_login_handler },
        { .uri = "/admin/settings", .method = HTTP_POST, .handler = admin_settings_handler },
        { .uri = "/admin/kick",     .method = HTTP_POST, .handler = admin_kick_handler },
        { .uri = "/admin/userspeed",.method = HTTP_POST, .handler = admin_userspeed_handler },
        { .uri = "/admin/usertime", .method = HTTP_POST, .handler = admin_usertime_handler },
        { .uri = "/admin/userdata", .method = HTTP_POST, .handler = admin_userdata_handler },
        { .uri = "/admin/exempt",   .method = HTTP_POST, .handler = admin_exempt_handler },
        { .uri = "/admin/resettime",.method = HTTP_POST, .handler = admin_resettime_handler },
        { .uri = "/admin/rename",   .method = HTTP_POST, .handler = admin_rename_handler },
        { .uri = "/admin/forget",   .method = HTTP_POST, .handler = admin_forget_handler },
        { .uri = "/admin/password", .method = HTTP_GET,  .handler = admin_password_get_handler },
        { .uri = "/admin/password", .method = HTTP_POST, .handler = admin_password_handler },
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
