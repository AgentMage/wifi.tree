#include "client_state.h"
#include "wifi_manager.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_CLIENTS 16

static client_t          s_clients[MAX_CLIENTS];
static SemaphoreHandle_t s_lock;

void clients_init(void) {
    memset(s_clients, 0, sizeof(s_clients));
    s_lock = xSemaphoreCreateMutex();
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
            return c;
        }
        if (!oldest || c->first_seen_us < oldest->first_seen_us) oldest = c;
    }
    // Table full — recycle the oldest entry.
    memset(oldest, 0, sizeof(*oldest));
    memcpy(oldest->mac, mac, 6);
    oldest->used = true;
    oldest->first_seen_us = esp_timer_get_time();
    return oldest;
}

// DHCP assigned an IP to a station — capture its hostname (option 12) if any.
static void on_ip_assigned(void *arg, esp_event_base_t base,
                           int32_t id, void *data) {
    ip_event_assigned_ip_to_client_t *e = (ip_event_assigned_ip_to_client_t *)data;
    if (e->hostname[0] == '\0') return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    client_t *c = get_or_create(e->mac);
    strlcpy(c->hostname, e->hostname, sizeof(c->hostname));
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
    xSemaphoreGive(s_lock);
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
