#pragma once

// Onboard LED (GPIO2, active HIGH on ESP32 DevKit V1).
//
// States:
//   LED_BLINK_FAST  200 ms  setup mode — device needs configuration
//   LED_BLINK_SLOW  1000 ms portal mode — waiting for uplink
//   LED_ON          solid   portal mode — internet up
//   LED_OFF         off     (unused in normal operation)

typedef enum { LED_OFF, LED_BLINK_FAST, LED_BLINK_SLOW, LED_ON } led_state_t;

void led_init(void);
void led_set(led_state_t state);

// Block the calling task and blink n times at period_ms, then restore prior state.
void led_blink_n(int n, int period_ms);
