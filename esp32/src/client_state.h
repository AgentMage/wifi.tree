#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// In-RAM, single-session client tracking. Keyed by MAC, capped at MAX_CLIENTS.
// State is lost on reboot by design — the ESP32 has no persistent user DB.

typedef struct {
    uint8_t  mac[6];
    char     name[41];       // visitor's chosen name (raw, escape on render)
    char     hostname[33];   // DHCP-reported hostname, "" if unknown
    int64_t  first_seen_us;  // esp_timer_get_time() at first sighting
    int64_t  leaf_grown_us;  // when the current leaf was grown, 0 = never
    bool     used;
} client_t;

void clients_init(void);

// Subscribe to DHCP "IP assigned to client" events so client hostnames
// (DHCP option 12) get recorded. Call once, after the default event loop exists.
void clients_start_hostname_capture(void);

// Resolve the client behind an AP-side IPv4 address (network byte order) to its
// table entry, creating one if the MAC is newly seen. NULL if the IP isn't a
// current AP station.
client_t *clients_find_by_ip(uint32_t ip_nbo);

// Record a freshly grown leaf for this client.
void clients_grow_leaf(client_t *c, const char *name);

// True if the client's leaf is still within ttl_seconds (ttl<=0 => never expires).
bool client_leaf_active(const client_t *c, int ttl_seconds);

// Whole seconds of freshness remaining (0 if expired/none, INT32_MAX if no TTL).
int client_leaf_seconds_left(const client_t *c, int ttl_seconds);

// Number of clients currently associated to the AP (for the admin page).
int clients_count(void);
