#include "authz.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define MAX_GRANTS 16

static struct { uint32_t ip; int64_t expiry_us; bool used; } s_grants[MAX_GRANTS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void authz_init(void) {
    memset(s_grants, 0, sizeof(s_grants));
}

void authz_grant(uint32_t ip_nbo, int64_t expiry_us) {
    taskENTER_CRITICAL(&s_mux);
    int slot = -1, freeslot = -1;
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (s_grants[i].used && s_grants[i].ip == ip_nbo) { slot = i; break; }
        if (!s_grants[i].used && freeslot < 0) freeslot = i;
    }
    if (slot < 0) slot = (freeslot >= 0) ? freeslot : 0; // overwrite slot 0 if full
    s_grants[slot].ip = ip_nbo;
    s_grants[slot].expiry_us = expiry_us;
    s_grants[slot].used = true;
    taskEXIT_CRITICAL(&s_mux);
}

void authz_revoke(uint32_t ip_nbo) {
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_GRANTS; i++)
        if (s_grants[i].used && s_grants[i].ip == ip_nbo) s_grants[i].used = false;
    taskEXIT_CRITICAL(&s_mux);
}

bool authz_allowed(uint32_t ip_nbo) {
    int64_t now = esp_timer_get_time();
    bool ok = false;
    taskENTER_CRITICAL(&s_mux);
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (s_grants[i].used && s_grants[i].ip == ip_nbo) {
            ok = (s_grants[i].expiry_us == 0 || now < s_grants[i].expiry_us);
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}
