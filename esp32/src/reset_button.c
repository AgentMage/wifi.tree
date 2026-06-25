#include "reset_button.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"

#define TAG        "reset"
#define RESET_GPIO GPIO_NUM_0   // BOOT button on ESP32 DevKit — active LOW
#define HOLD_MS    5000
#define POLL_MS    100

static void reset_task(void *arg) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << RESET_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int held_ms = 0;
    while (1) {
        if (gpio_get_level(RESET_GPIO) == 0) {
            held_ms += POLL_MS;
            if (held_ms >= HOLD_MS) {
                ESP_LOGW(TAG, "Factory reset triggered — clearing credentials and rebooting");
                nvs_handle_t h;
                if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_key(h, "ssid");
                    nvs_erase_key(h, "pass");
                    nvs_commit(h);
                    nvs_close(h);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void reset_button_start(void) {
    xTaskCreate(reset_task, "reset_btn", 2048, NULL, 3, NULL);
}
