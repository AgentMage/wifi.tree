#pragma once
// Injected into lwIP via -DESP_IDF_LWIP_HOOK_FILENAME. Gates IPv4 forwarding so
// only AP clients holding an active leaf reach the internet. Portal and DNS
// traffic is addressed to 10.42.0.1 (local) and never enters the forward path,
// so unregistered clients can still load the captive portal and resolve names.

#include "lwip/pbuf.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/def.h"
#include "authz.h"
#include "shaper.h"

// Gates and rate-limits forwarded IPv4 traffic per client. `dest` is the
// destination address in host byte order (as ip4_canforward passes it).
// Returns 1 = forward, 0 = drop, -1 = let lwIP's default logic decide.
static inline int wifitree_ip4_canforward(struct pbuf *p, u32_t dest) {
    u32_t src = ip4_addr_get_u32(ip4_current_src_addr()); // network byte order
    const uint8_t *b = (const uint8_t *)&src;

    if (b[0] == 10 && b[1] == 42 && b[2] == 0) {          // upload: client → internet
        if (!authz_allowed(src)) return 0;
        return shaper_admit(src, p->tot_len, 1) ? 1 : 0;
    }
    if ((dest & 0xFFFFFF00u) == 0x0A2A0000u) {            // download: internet → client
        u32_t client = lwip_htonl(dest);                  // back to network byte order
        if (!authz_allowed(client)) return 0;
        return shaper_admit(client, p->tot_len, 0) ? 1 : 0;
    }
    return -1;                                            // not AP-client traffic
}

#define LWIP_HOOK_IP4_CANFORWARD(p, addr) wifitree_ip4_canforward((p), (addr))
