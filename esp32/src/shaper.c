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
    bool     used;
} entry_t;

static entry_t      s_tab[MAX_CLIENTS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void shaper_init(void) {
    memset(s_tab, 0, sizeof(s_tab));
}

bool shaper_admit(uint32_t ip_nbo, uint16_t len, int dir) {
    int bps = config_client_kbps() * 125;   // kbit/s → bytes/s (1 kbit = 125 B)
    if (bps <= 0) return true;               // 0 = uncapped
    const int64_t burst = bps;               // 1 second of credit
    int64_t now = esp_timer_get_time();
    bool admit;

    taskENTER_CRITICAL(&s_mux);
    entry_t *e = NULL, *freeslot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_tab[i].used && s_tab[i].ip == ip_nbo) { e = &s_tab[i]; break; }
        if (!s_tab[i].used && !freeslot) freeslot = &s_tab[i];
        if (s_tab[i].used && (!oldest || s_tab[i].last_us[0] < oldest->last_us[0]))
            oldest = &s_tab[i];
    }
    if (!e) {                                // first packet for this client
        e = freeslot ? freeslot : oldest;
        e->ip = ip_nbo;
        e->used = true;
        e->tokens[0] = e->tokens[1] = burst;
        e->last_us[0] = e->last_us[1] = now;
    }
    int64_t elapsed = now - e->last_us[dir];
    if (elapsed > 0) {
        e->tokens[dir] += elapsed * bps / 1000000;
        if (e->tokens[dir] > burst) e->tokens[dir] = burst;
        e->last_us[dir] = now;
    }
    if (e->tokens[dir] >= len) { e->tokens[dir] -= len; admit = true; }
    else                         admit = false;
    taskEXIT_CRITICAL(&s_mux);
    return admit;
}
