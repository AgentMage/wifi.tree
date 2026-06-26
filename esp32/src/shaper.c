#include "shaper.h"
#include "config.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define MAX_CLIENTS 16

// Two token buckets per client: index 0 = download, 1 = upload. Tokens are in
// bytes; refilled at the configured rate, capped at one second's worth (burst).
typedef struct {
    uint32_t ip;
    int64_t  tokens[2];
    int64_t  last_us[2];
    int      override_kbps;  // -1 = use global default
    uint64_t bytes;          // forwarded bytes (up+down) since last budget drain
    uint64_t cum[2];         // cumulative forwarded bytes [0]=down [1]=up (live view)
    bool     used;
} entry_t;

static entry_t      s_tab[MAX_CLIENTS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void shaper_init(void) {
    memset(s_tab, 0, sizeof(s_tab));
}

// Locate (or allocate) the entry for an IP. Caller holds s_mux.
static entry_t *find_or_make(uint32_t ip_nbo, int64_t now) {
    entry_t *freeslot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_tab[i].used && s_tab[i].ip == ip_nbo) return &s_tab[i];
        if (!s_tab[i].used && !freeslot) freeslot = &s_tab[i];
        if (s_tab[i].used && (!oldest || s_tab[i].last_us[0] < oldest->last_us[0]))
            oldest = &s_tab[i];
    }
    entry_t *e = freeslot ? freeslot : oldest;
    e->ip = ip_nbo;
    e->used = true;
    e->override_kbps = -1;
    e->bytes = 0;
    e->cum[0] = e->cum[1] = 0;
    e->tokens[0] = e->tokens[1] = 0;
    e->last_us[0] = e->last_us[1] = 0; // first packet accrues a full burst of credit
    (void)now;
    return e;
}

bool shaper_admit(uint32_t ip_nbo, uint16_t len, int dir) {
    int64_t now = esp_timer_get_time();
    bool admit;

    taskENTER_CRITICAL(&s_mux);
    entry_t *e = find_or_make(ip_nbo, now);
    int kbps = e->override_kbps >= 0 ? e->override_kbps : config_client_kbps();
    if (kbps <= 0) {                         // 0 = uncapped
        e->bytes += len;                     // still metered for the data budget
        e->cum[dir] += len;
        taskEXIT_CRITICAL(&s_mux);
        return true;
    }
    int64_t bps = (int64_t)kbps * 125;       // kbit/s → bytes/s
    int64_t burst = bps;                     // 1 second of credit
    int64_t elapsed = now - e->last_us[dir];
    if (elapsed > 0) {
        e->tokens[dir] += elapsed * bps / 1000000;
        if (e->tokens[dir] > burst) e->tokens[dir] = burst;
        e->last_us[dir] = now;
    }
    if (e->tokens[dir] >= len) { e->tokens[dir] -= len; admit = true; e->bytes += len; e->cum[dir] += len; }
    else                         admit = false;
    taskEXIT_CRITICAL(&s_mux);
    return admit;
}

void shaper_get_totals(uint32_t ip_nbo, uint64_t *down, uint64_t *up) {
    uint64_t d = 0, u = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_tab[i].used && s_tab[i].ip == ip_nbo) {
            d = s_tab[i].cum[0];
            u = s_tab[i].cum[1];
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (down) *down = d;
    if (up)   *up = u;
}

uint64_t shaper_take_bytes(uint32_t ip_nbo) {
    uint64_t b = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_tab[i].used && s_tab[i].ip == ip_nbo) {
            b = s_tab[i].bytes;
            s_tab[i].bytes = 0;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return b;
}

void shaper_set_override(uint32_t ip_nbo, int kbps) {
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    entry_t *e = find_or_make(ip_nbo, now);
    e->override_kbps = kbps;
    taskEXIT_CRITICAL(&s_mux);
}
