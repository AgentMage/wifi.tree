#pragma once

// Starts a background task that monitors GPIO0 (the BOOT button on ESP32 DevKit).
// Holding it LOW for 5 seconds clears NVS Wi-Fi credentials and reboots into setup mode.
void reset_button_start(void);
