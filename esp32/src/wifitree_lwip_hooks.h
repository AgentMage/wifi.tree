#pragma once
// Injected into lwIP via -DESP_IDF_LWIP_HOOK_FILENAME. Gates IPv4 forwarding so
// only AP clients holding an active leaf reach the internet. Portal and DNS
// traffic is addressed to 10.42.0.1 (local) and never enters the forward path,
// so unregistered clients can still load the captive portal and resolve names.

#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "authz.h"

// Return 1 = forward, 0 = drop, -1 = let lwIP's default logic decide.
static inline int wifitree_ip4_canforward(struct pbuf *p, u32_t dest) {
    (void)p; (void)dest;
    u32_t src = ip4_addr_get_u32(ip4_current_src_addr()); // network byte order
    const uint8_t *b = (const uint8_t *)&src;
    if (b[0] == 10 && b[1] == 42 && b[2] == 0)            // AP subnet 10.42.0.0/24
        return authz_allowed(src) ? 1 : 0;
    return -1;                                            // not an AP client
}

#define LWIP_HOOK_IP4_CANFORWARD(p, addr) wifitree_ip4_canforward((p), (addr))
