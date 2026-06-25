#include "led.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_GPIO GPIO_NUM_2

static volatile led_state_t s_state = LED_OFF;

static void led_task(void *arg) {
    bool on = false;
    while (1) {
        switch (s_state) {
            case LED_ON:
                gpio_set_level(LED_GPIO, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case LED_OFF:
                gpio_set_level(LED_GPIO, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case LED_BLINK_FAST:
                on = !on;
                gpio_set_level(LED_GPIO, on);
                vTaskDelay(pdMS_TO_TICKS(200));
                break;
            case LED_BLINK_SLOW:
                on = !on;
                gpio_set_level(LED_GPIO, on);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
        }
    }
}

void led_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, 0);
    xTaskCreate(led_task, "led", 1024, NULL, 2, NULL);
}

void led_set(led_state_t state) {
    s_state = state;
}

void led_blink_n(int n, int period_ms) {
    led_state_t prev = s_state;
    s_state = LED_OFF;
    for (int i = 0; i < n; i++) {
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(period_ms / 2));
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(period_ms / 2));
    }
    s_state = prev;
}
