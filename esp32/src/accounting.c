#include "accounting.h"
#include "client_state.h"
#include "config.h"
#include "authz.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG          "acct"
#define TICK_SECONDS 30

// Every TICK_SECONDS: credit online visitors their elapsed time, cut off anyone
// who has reached the connected-time cap, then persist the table if it changed.
static void accounting_task(void *arg) {
    ESP_LOGI(TAG, "connected-time accounting started (%ds tick)", TICK_SECONDS);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TICK_SECONDS * 1000));

        int ttl = config_leaf_ttl_seconds();
        int cap = config_connected_cap_seconds();
        uint64_t data_cap = (uint64_t)config_data_cap_mb() << 20;  // MB → bytes

        uint32_t banned[16];
        int nb = 0;
        clients_account_and_enforce(TICK_SECONDS, ttl, cap, data_cap, banned, 16, &nb);

        for (int i = 0; i < nb; i++) {
            authz_revoke(banned[i]);   // close the internet gate immediately
            ESP_LOGI(TAG, "visitor %lu hit a usage budget — cut off",
                     (unsigned long)banned[i]);
        }

        clients_flush();               // no-op when nothing changed
    }
}

void accounting_start(void) {
    // 5 KB stack: the periodic clients_flush() does an NVS/flash write, which
    // needs comfortable headroom beyond the default.
    xTaskCreate(accounting_task, "accounting", 5120, NULL, 2, NULL);
}
