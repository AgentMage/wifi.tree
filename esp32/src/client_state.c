#include "client_state.h"
#include "wifi_manager.h"
#include "shaper.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_CLIENTS 16
#define NVS_NS  "users"
#define NVS_KEY "table"
#define TAG     "clients"
#define PERSIST_VER 3          // bump when persist_rec_t layout changes

static client_t          s_clients[MAX_CLIENTS];
static SemaphoreHandle_t s_lock;
static volatile bool     s_dirty;   // table changed since last flush

// On-flash record — only the persistent fields, packed for a stable layout.
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    char     name[41];
    char     hostname[33];
    uint32_t total_connected_s;
    uint64_t total_bytes;
    uint8_t  banned;
    int32_t  bw_cap_kbps;       // per-user speed cap: -1 = global default, 0 = uncapped
} persist_rec_t;

// Blob layout: [persist_hdr_t][persist_rec_t × count]. The version lets a
// firmware with a changed record layout safely ignore an old-format blob.
typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t count;
} persist_hdr_t;

// Mark the table as needing a flush. Caller may or may not hold s_lock; setting
// a bool is atomic enough that it doesn't matter.
static inline void mark_dirty(void) { s_dirty = true; }

void clients_init(void) {
    memset(s_clients, 0, sizeof(s_clients));
    s_lock = xSemaphoreCreateMutex();
    s_dirty = false;

    // Restore persisted records (identity + lifetime); ephemeral fields stay 0.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t buf[sizeof(persist_hdr_t) + MAX_CLIENTS * sizeof(persist_rec_t)];
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, NVS_KEY, buf, &len) == ESP_OK && len >= sizeof(persist_hdr_t)) {
        persist_hdr_t hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.version == PERSIST_VER) {
            int n = hdr.count;
            if (n > MAX_CLIENTS) n = MAX_CLIENTS;
            for (int i = 0; i < n; i++) {
                persist_rec_t r;
                memcpy(&r, buf + sizeof(hdr) + i * sizeof(r), sizeof(r));
                client_t *c = &s_clients[i];
                memcpy(c->mac, r.mac, 6);
                strlcpy(c->name, r.name, sizeof(c->name));
                strlcpy(c->hostname, r.hostname, sizeof(c->hostname));
                c->total_connected_s = r.total_connected_s;
                c->total_bytes = r.total_bytes;
                c->banned = r.banned;
                c->bw_cap_kbps = r.bw_cap_kbps;
                c->first_seen_us = esp_timer_get_time();
                c->used = true;
            }
            ESP_LOGI(TAG, "restored %d user record(s) from flash", n);
        } else {
            ESP_LOGW(TAG, "ignoring user blob (version %d, want %d)",
                     hdr.version, PERSIST_VER);
        }
    }
    nvs_close(h);
}

void clients_flush(void) {
    if (!s_dirty) return;

    // Build the blob under the lock, then write outside it so the (slowish) NVS
    // commit doesn't block other client-table access.
    uint8_t buf[sizeof(persist_hdr_t) + MAX_CLIENTS * sizeof(persist_rec_t)];
    uint16_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!s_clients[i].used) continue;
        persist_rec_t r = {0};
        memcpy(r.mac, s_clients[i].mac, 6);
        strlcpy(r.name, s_clients[i].name, sizeof(r.name));
        strlcpy(r.hostname, s_clients[i].hostname, sizeof(r.hostname));
        r.total_connected_s = s_clients[i].total_connected_s;
        r.total_bytes = s_clients[i].total_bytes;
        r.banned = s_clients[i].banned;
        r.bw_cap_kbps = s_clients[i].bw_cap_kbps;
        memcpy(buf + sizeof(persist_hdr_t) + n * sizeof(r), &r, sizeof(r));
        n++;
    }
    s_dirty = false;
    xSemaphoreGive(s_lock);

    persist_hdr_t hdr = { .version = PERSIST_VER, .count = n };
    memcpy(buf, &hdr, sizeof(hdr));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        s_dirty = true;   // retry on the next tick
        return;
    }
    size_t blob_len = sizeof(persist_hdr_t) + n * sizeof(persist_rec_t);
    if (nvs_set_blob(h, NVS_KEY, buf, blob_len) == ESP_OK)
        nvs_commit(h);
    else
        s_dirty = true;
    nvs_close(h);
}

// Find the table slot for a MAC, allocating (or evicting the oldest) as needed.
// Caller must hold s_lock.
static client_t *get_or_create(const uint8_t mac[6]) {
    client_t *oldest = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (c->used && memcmp(c->mac, mac, 6) == 0) return c;
        if (!c->used) {
            memset(c, 0, sizeof(*c));
            memcpy(c->mac, mac, 6);
            c->used = true;
            c->bw_cap_kbps = -1;        // use global default until set
            c->first_seen_us = esp_timer_get_time();
            mark_dirty();
            return c;
        }
        if (!oldest || c->first_seen_us < oldest->first_seen_us) oldest = c;
    }
    // Table full — recycle the oldest entry.
    memset(oldest, 0, sizeof(*oldest));
    memcpy(oldest->mac, mac, 6);
    oldest->used = true;
    oldest->bw_cap_kbps = -1;
    oldest->first_seen_us = esp_timer_get_time();
    mark_dirty();
    return oldest;
}

// DHCP assigned an IP to a station — capture its hostname (option 12) if any.
static void on_ip_assigned(void *arg, esp_event_base_t base,
                           int32_t id, void *data) {
    ip_event_assigned_ip_to_client_t *e = (ip_event_assigned_ip_to_client_t *)data;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    client_t *c = get_or_create(e->mac);
    c->ip = e->ip.addr;
    if (e->hostname[0] && strcmp(c->hostname, e->hostname) != 0) {
        strlcpy(c->hostname, e->hostname, sizeof(c->hostname));
        mark_dirty();   // hostname is persistent
    }
    int cap = c->bw_cap_kbps;
    uint32_t ip = c->ip;
    xSemaphoreGive(s_lock);

    // Re-apply this device's persisted speed cap to the shaper for its (new) IP
    // (-1 clears any stale override left on that IP, falling back to the global).
    shaper_set_override(ip, cap);
}

void clients_start_hostname_capture(void) {
    esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                               on_ip_assigned, NULL);
}

client_t *clients_find_by_ip(uint32_t ip_nbo) {
    wifi_sta_list_t wifi_sta;
    if (esp_wifi_ap_get_sta_list(&wifi_sta) != ESP_OK || wifi_sta.num == 0) return NULL;

    // Resolve each associated MAC to its DHCP-assigned IP.
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
    int num = wifi_sta.num;
    for (int i = 0; i < num; i++)
        memcpy(pairs[i].mac, wifi_sta.sta[i].mac, 6);

    esp_netif_t *ap = wifi_ap_netif();
    if (!ap || esp_netif_dhcps_get_clients_by_mac(ap, num, pairs) != ESP_OK) return NULL;

    for (int i = 0; i < num; i++) {
        if (pairs[i].ip.addr == ip_nbo) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            client_t *c = get_or_create(pairs[i].mac);
            c->ip = ip_nbo;
            xSemaphoreGive(s_lock);
            return c;
        }
    }
    return NULL;
}

void clients_grow_leaf(client_t *c, const char *name) {
    if (!c) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(c->name, name, sizeof(c->name));
    c->leaf_grown_us = esp_timer_get_time();
    mark_dirty();   // name is persistent
    xSemaphoreGive(s_lock);
    clients_flush();   // registration is discrete & rare — persist it now
}

bool client_leaf_active(const client_t *c, int ttl_seconds) {
    if (!c || c->leaf_grown_us == 0) return false;
    if (ttl_seconds <= 0) return true; // 0 => leaves never expire
    int64_t age_s = (esp_timer_get_time() - c->leaf_grown_us) / 1000000;
    return age_s < ttl_seconds;
}

int client_leaf_seconds_left(const client_t *c, int ttl_seconds) {
    if (!c || c->leaf_grown_us == 0) return 0;
    if (ttl_seconds <= 0) return INT32_MAX;
    int64_t age_s = (esp_timer_get_time() - c->leaf_grown_us) / 1000000;
    int64_t left = ttl_seconds - age_s;
    return left > 0 ? (int)left : 0;
}

int clients_live_view(client_view_t *out, int max) {
    wifi_sta_list_t sl;
    if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK || sl.num == 0) return 0;

    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
    int num = sl.num;
    for (int i = 0; i < num; i++) memcpy(pairs[i].mac, sl.sta[i].mac, 6);

    esp_netif_t *ap = wifi_ap_netif();
    if (!ap || esp_netif_dhcps_get_clients_by_mac(ap, num, pairs) != ESP_OK) return 0;

    int outn = 0;
    for (int i = 0; i < num && outn < max; i++) {
        client_view_t *v = &out[outn];
        memset(v, 0, sizeof(*v));
        v->ip   = pairs[i].ip.addr;
        v->rssi = sl.sta[i].rssi;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int j = 0; j < MAX_CLIENTS; j++) {
            if (s_clients[j].used && memcmp(s_clients[j].mac, sl.sta[i].mac, 6) == 0) {
                strlcpy(v->name, s_clients[j].name, sizeof(v->name));
                strlcpy(v->hostname, s_clients[j].hostname, sizeof(v->hostname));
                v->total_connected_s = s_clients[j].total_connected_s;
                break;
            }
        }
        xSemaphoreGive(s_lock);

        if (v->ip) shaper_get_totals(v->ip, &v->down, &v->up);
        outn++;
    }
    return outn;
}

int clients_count(void) {
    wifi_sta_list_t wifi_sta;
    if (esp_wifi_ap_get_sta_list(&wifi_sta) != ESP_OK) return 0;
    return wifi_sta.num;
}

void clients_clear_leaf_by_ip(uint32_t ip_nbo) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (s_clients[i].used && s_clients[i].ip == ip_nbo)
            s_clients[i].leaf_grown_us = 0;
    xSemaphoreGive(s_lock);
}

void clients_set_name_by_mac(const uint8_t mac[6], const char *name) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].used && memcmp(s_clients[i].mac, mac, 6) == 0) {
            strlcpy(s_clients[i].name, name, sizeof(s_clients[i].name));
            mark_dirty();
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

uint32_t clients_remove_by_mac(const uint8_t mac[6]) {
    uint32_t ip = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].used && memcmp(s_clients[i].mac, mac, 6) == 0) {
            ip = s_clients[i].ip;
            memset(&s_clients[i], 0, sizeof(s_clients[i]));
            mark_dirty();
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ip;
}

uint32_t clients_set_bw_cap_by_mac(const uint8_t mac[6], int kbps) {
    uint32_t ip = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].used && memcmp(s_clients[i].mac, mac, 6) == 0) {
            s_clients[i].bw_cap_kbps = kbps;
            ip = s_clients[i].ip;
            mark_dirty();
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ip;
}

void clients_reset_budget_by_mac(const uint8_t mac[6]) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].used && memcmp(s_clients[i].mac, mac, 6) == 0) {
            s_clients[i].total_connected_s = 0;
            s_clients[i].total_bytes = 0;
            s_clients[i].banned = false;
            mark_dirty();
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

void clients_account_and_enforce(int elapsed_s, int ttl_s, int cap_s,
                                 uint64_t data_cap_bytes,
                                 uint32_t *banned_out, int max, int *n_out) {
    int nb = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &s_clients[i];
        if (!c->used) continue;

        // Fold this device's forwarded bytes since the last tick into its
        // lifetime total (done for everyone, to catch trailing usage).
        if (c->ip) {
            uint64_t b = shaper_take_bytes(c->ip);
            if (b) { c->total_bytes += b; mark_dirty(); }
        }
        if (c->banned) continue;

        bool over = false;
        if (client_leaf_active(c, ttl_s)) {            // only online users accrue time
            c->total_connected_s += elapsed_s;
            mark_dirty();
            if (cap_s > 0 && c->total_connected_s >= (uint32_t)cap_s) over = true;
        }
        if (data_cap_bytes > 0 && c->total_bytes >= data_cap_bytes) over = true;

        if (over) {
            c->banned = true;
            c->leaf_grown_us = 0;                      // close their session
            if (nb < max && c->ip) banned_out[nb++] = c->ip;
        }
    }
    xSemaphoreGive(s_lock);
    if (n_out) *n_out = nb;
}

int clients_snapshot(client_t *out, int max) {
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
        if (s_clients[i].used) out[n++] = s_clients[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}
