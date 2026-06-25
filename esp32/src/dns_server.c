#include "dns_server.h"
#include "wifi_manager.h"
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#define TAG    "dns"
#define DNS_PORT 53
#define BUF_SZ   512

static uint32_t s_ip;
static bool     s_spoof_all;

// Domains that trigger the OS captive-portal popup — these get spoofed to
// our AP IP so the portal overlay appears.  Everything else is forwarded to
// 8.8.8.8 so real internet DNS works once NAT is up.
static const char *CAPTIVE_DOMAINS[] = {
    "wifi.tree",                        // our own hostname
    "captive.apple.com",
    "www.apple.com",
    "connectivitycheck.gstatic.com",
    "connectivitycheck.android.com",
    "clients3.google.com",
    "www.msftconnecttest.com",
    "www.msftncsi.com",
    "ipv6.msftconnecttest.com",
    "detectportal.firefox.com",
    "nmcheck.gnome.org",
    NULL,
};

// Parse the DNS wire-format name starting at offset 12 into a dotted string.
// Returns the name length or -1 on error.
static int extract_domain(const uint8_t *pkt, int pktlen, char *out, int outsz) {
    int pos = 12, n = 0;
    while (pos < pktlen && pkt[pos] != 0) {
        int len = pkt[pos++];
        if (len > 63 || n + len + 1 >= outsz || pos + len > pktlen) return -1;
        if (n > 0) out[n++] = '.';
        memcpy(out + n, pkt + pos, len);
        n += len;
        pos += len;
    }
    out[n] = '\0';
    return n;
}

static bool is_captive_domain(const char *domain) {
    for (int i = 0; CAPTIVE_DOMAINS[i]; i++) {
        if (strcasecmp(domain, CAPTIVE_DOMAINS[i]) == 0) return true;
    }
    return false;
}

// Build a spoofed A reply pointing to s_ip.
static int make_spoof_reply(const uint8_t *q, int qlen, uint8_t *r) {
    if (qlen < 12 || qlen + 16 > BUF_SZ) return -1;
    memcpy(r, q, qlen);
    r[2] = 0x81; r[3] = 0x80;
    r[6] = 0;    r[7] = 1;
    r[8] = 0;    r[9] = 0;
    r[10] = 0;   r[11] = 0;
    int p = qlen;
    r[p++] = 0xC0; r[p++] = 0x0C;
    r[p++] = 0x00; r[p++] = 0x01;
    r[p++] = 0x00; r[p++] = 0x01;
    r[p++] = 0x00; r[p++] = 0x00;
    r[p++] = 0x00; r[p++] = 60;
    r[p++] = 0x00; r[p++] = 0x04;
    memcpy(r + p, &s_ip, 4); p += 4;
    return p;
}

// Build a SERVFAIL reply (rcode=2) so the client fails fast without waiting for a timeout.
static int make_servfail(const uint8_t *q, int qlen, uint8_t *r) {
    if (qlen < 12) return -1;
    memcpy(r, q, qlen);
    r[2] = 0x81; r[3] = 0x82; // QR=1, AA=0, RCODE=2 (SERVFAIL)
    r[6] = 0;    r[7] = 0;    // ancount = 0
    return qlen;
}

// Forward query to 8.8.8.8 and relay the reply.  Returns reply length or -1.
static int forward_query(const uint8_t *query, int qlen, uint8_t *reply) {
    if (!wifi_has_uplink()) return make_servfail(query, qlen, reply);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in upstream = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = inet_addr("8.8.8.8"),
    };
    sendto(sock, query, qlen, 0, (struct sockaddr *)&upstream, sizeof(upstream));
    int rlen = recv(sock, reply, BUF_SZ, 0);
    close(sock);
    return rlen;
}

static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed"); close(sock); vTaskDelete(NULL); return;
    }

    ESP_LOGI(TAG, "DNS ready (mode: %s)", s_spoof_all ? "spoof-all" : "selective+forward");
    uint8_t buf[BUF_SZ], rep[BUF_SZ + 16];
    struct sockaddr_in client;
    socklen_t clen;

    while (1) {
        clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (n < 12) continue;

        bool spoof = s_spoof_all;
        if (!spoof) {
            char domain[128];
            if (extract_domain(buf, n, domain, sizeof(domain)) > 0) {
                spoof = is_captive_domain(domain);
                if (spoof) ESP_LOGI(TAG, "spoof: %s", domain);
                else       ESP_LOGD(TAG, "fwd:   %s", domain);
            } else {
                spoof = true; // can't parse → spoof defensively
            }
        }

        if (spoof) {
            int rlen = make_spoof_reply(buf, n, rep);
            if (rlen > 0)
                sendto(sock, rep, rlen, 0, (struct sockaddr *)&client, clen);
        } else {
            int rlen = forward_query(buf, n, rep);
            if (rlen > 0)
                sendto(sock, rep, rlen, 0, (struct sockaddr *)&client, clen);
        }
    }
}

void dns_server_start(uint32_t ap_ip, bool spoof_all) {
    s_ip        = ap_ip;
    s_spoof_all = spoof_all;
    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);
}
