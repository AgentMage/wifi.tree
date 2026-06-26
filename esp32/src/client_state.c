#include "client_state.h"
#include "wifi_manager.h"
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

static client_t          s_clients[MAX_CLIENTS];
static SemaphoreHandle_t s_lock;
static volatile bool     s_dirty;   // table changed since last flush

// On-flash record — only the persistent fields, packed for a stable layout.
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    char     name[41];
    char     hostname[33];
    uint32_t total_connected_s;
    uint8_t  banned;
} persist_rec_t;

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
    persist_rec_t recs[MAX_CLIENTS];
    size_t len = sizeof(recs);
    if (nvs_get_blob(h, NVS_KEY, recs, &len) == ESP_OK) {
        int n = len / sizeof(persist_rec_t);
        if (n > MAX_CLIENTS) n = MAX_CLIENTS;
        for (int i = 0; i < n; i++) {
            client_t *c = &s_clients[i];
            memcpy(c->mac, recs[i].mac, 6);
            strlcpy(c->name, recs[i].name, sizeof(c->name));
            strlcpy(c->hostname, recs[i].hostname, sizeof(c->hostname));
            c->total_connected_s = recs[i].total_connected_s;
            c->banned = recs[i].banned;
            c->first_seen_us = esp_timer_get_time();
            c->used = true;
        }
        ESP_LOGI(TAG, "restored %d user record(s) from flash", n);
    }
    nvs_close(h);
}

void clients_flush(void) {
    if (!s_dirty) return;

    // Snapshot the persistent fields under the lock, then write outside it so
    // the (slowish) NVS commit doesn't block other client-table access.
    persist_rec_t recs[MAX_CLIENTS];
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!s_clients[i].used) continue;
        memcpy(recs[n].mac, s_clients[i].mac, 6);
        strlcpy(recs[n].name, s_clients[i].name, sizeof(recs[n].name));
        strlcpy(recs[n].hostname, s_clients[i].hostname, sizeof(recs[n].hostname));
        recs[n].total_connected_s = s_clients[i].total_connected_s;
        recs[n].banned = s_clients[i].banned;
        n++;
    }
    s_dirty = false;
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        s_dirty = true;   // retry on the next tick
        return;
    }
    if (nvs_set_blob(h, NVS_KEY, recs, n * sizeof(persist_rec_t)) == ESP_OK)
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
    xSemaphoreGive(s_lock);
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

int clients_snapshot(client_t *out, int max) {
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
        if (s_clients[i].used) out[n++] = s_clients[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}
