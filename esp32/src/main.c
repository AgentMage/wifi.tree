#include "wifi_manager.h"
#include "dns_server.h"
#include "http_server.h"
#include "reset_button.h"
#include "esp_log.h"

#define TAG "main"

void app_main(void) {
    nvs_init();

    char ssid[64] = {0}, pass[64] = {0};
    bool has_creds = wifi_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    reset_button_start(); // GPIO0 hold 5s → clear credentials + reboot into setup mode

    if (!has_creds) {
        ESP_LOGI(TAG, "=== SETUP MODE ===");
        const char *networks = wifi_start_setup();
        dns_server_start(wifi_ap_ip(), true);   // spoof everything — no internet anyway
        http_server_start_setup(networks);
        ESP_LOGI(TAG, "Connect to '%s' and open any page", "WifiTree-Setup");
    } else {
        ESP_LOGI(TAG, "=== PORTAL MODE === uplink: %s", ssid);
        wifi_start_portal(ssid, pass);
        dns_server_start(wifi_ap_ip(), false);  // spoof captive domains, forward the rest
        http_server_start_portal();
        ESP_LOGI(TAG, "Connect to '%s'", "WifiTree");
    }
}
