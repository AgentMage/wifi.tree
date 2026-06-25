#include "wifi_manager.h"
#include "dns_server.h"
#include "http_server.h"
#include "client_state.h"
#include "reset_button.h"
#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "main"

// Portal mode: poll uplink status every second and update the LED accordingly.
static void portal_led_task(void *arg) {
    while (1) {
        led_set(wifi_has_uplink() ? LED_ON : LED_BLINK_SLOW);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    nvs_init();
    led_init();
    clients_init();

    char ssid[64] = {0}, pass[64] = {0};
    bool has_creds = wifi_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    reset_button_start(); // GPIO0 hold 5s → clear credentials + reboot into setup mode

    if (!has_creds) {
        ESP_LOGI(TAG, "=== SETUP MODE ===");
        led_set(LED_BLINK_FAST);
        const char *networks = wifi_start_setup();
        dns_server_start(wifi_ap_ip(), true);   // spoof everything — no internet anyway
        http_server_start_setup(networks);
        ESP_LOGI(TAG, "Connect to '%s' and open any page", "wifi.tree-setup");
    } else {
        ESP_LOGI(TAG, "=== PORTAL MODE === uplink: %s", ssid);
        led_set(LED_BLINK_SLOW);
        wifi_start_portal(ssid, pass);
        dns_server_start(wifi_ap_ip(), false);  // spoof captive domains, forward the rest
        http_server_start_portal();
        xTaskCreate(portal_led_task, "portal_led", 1024, NULL, 2, NULL);
        ESP_LOGI(TAG, "Connect to '%s'", "wifi.tree");
    }
}
