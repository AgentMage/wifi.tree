#pragma once
#include <stdbool.h>
#include <stdint.h>

// Per-client internet allow-list, consulted by the lwIP forwarding hook to
// decide whether an AP client's packets may be NAT'd to the uplink. Kept tiny
// and spinlock-guarded so it's safe to read from the tcpip thread.

void authz_init(void);

// Allow IP (network byte order) until expiry_us (esp_timer_get_time microseconds);
// expiry_us == 0 means no expiry. Re-granting the same IP refreshes it.
void authz_grant(uint32_t ip_nbo, int64_t expiry_us);

// Remove any grant for IP.
void authz_revoke(uint32_t ip_nbo);

// True if IP currently holds an unexpired grant. Safe from the lwIP thread.
bool authz_allowed(uint32_t ip_nbo);
