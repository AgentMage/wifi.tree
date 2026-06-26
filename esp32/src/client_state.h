#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Per-MAC client tracking, capped at MAX_CLIENTS. The identity and lifetime
// fields persist across reboots in NVS (namespace "users"); the ephemeral
// fields are RAM-only and reset to 0 on boot (so leaves still reset on reboot).

typedef struct {
    // ── Persistent (saved to NVS) ──
    uint8_t  mac[6];
    char     name[41];          // visitor's chosen name (raw, escape on render)
    char     hostname[33];      // DHCP-reported hostname, "" if unknown
    uint32_t total_connected_s; // lifetime seconds spent online (time budget)
    bool     banned;            // true => cut off (over budget or kicked-for-good)
    // ── Ephemeral (RAM only, zeroed on boot) ──
    uint32_t ip;             // last-seen IP (network byte order), 0 if unknown
    int64_t  first_seen_us;  // esp_timer_get_time() at first sighting
    int64_t  leaf_grown_us;  // when the current leaf was grown, 0 = never
    bool     used;
} client_t;

// Load any persisted records from NVS and prepare the in-RAM table.
void clients_init(void);

// Write the table to NVS if it has changed since the last flush (no-op when
// clean). Cheap when unchanged; call periodically (e.g. the 30s accounting tick).
void clients_flush(void);

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

// Copy up to max used table entries into out. Returns the number copied.
int clients_snapshot(client_t *out, int max);

// Clear the leaf of whichever client currently holds this IP (network byte
// order), so they revert to the new-visitor flow. No-op if not found.
void clients_clear_leaf_by_ip(uint32_t ip_nbo);

// Accounting tick: credit `elapsed_s` of online time to every visitor whose
// leaf is currently active (ttl_s = leaf TTL used to judge "online"). If
// cap_s > 0, any visitor reaching the cap is banned and their leaf cleared;
// the IPs of newly-banned visitors are written to banned_out (up to `max`) and
// *n_out is set, so the caller can revoke their internet grants. Marks the
// table dirty when totals change (caller should clients_flush() afterward).
void clients_account_and_enforce(int elapsed_s, int ttl_s, int cap_s,
                                 uint32_t *banned_out, int max, int *n_out);
