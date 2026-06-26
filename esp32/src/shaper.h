#pragma once
#include <stdbool.h>
#include <stdint.h>

// Per-client token-bucket rate limiter, consulted by the lwIP forwarding hook.
// Separate up/down buckets per client IP. Safe to call from the tcpip thread.

void shaper_init(void);

// Admit/deny a forwarded packet of `len` bytes for client `ip_nbo`
// (network byte order). dir: 0 = download (to client), 1 = upload (from client).
// Returns true to forward, false to drop. Rate comes from config (0 = uncapped).
bool shaper_admit(uint32_t ip_nbo, uint16_t len, int dir);

// Override the per-client rate for one IP (kbit/s; 0 = uncapped, -1 = clear
// override and use the global default).
void shaper_set_override(uint32_t ip_nbo, int kbps);

// Return the forwarded-byte count accumulated for IP (up + down) since the last
// call, and zero it. Used by the accounting tick to fold usage into the
// persistent per-visitor total. Returns 0 if the IP has no live entry.
uint64_t shaper_take_bytes(uint32_t ip_nbo);

// Read the cumulative forwarded byte totals for IP (down/up), for the live
// dashboard. These are not zeroed (until the entry is recycled). 0 if no entry.
void shaper_get_totals(uint32_t ip_nbo, uint64_t *down, uint64_t *up);
