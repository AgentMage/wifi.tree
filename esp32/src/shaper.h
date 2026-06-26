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
