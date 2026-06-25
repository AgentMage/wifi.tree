#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AP_IP_STR   "10.42.0.1"
#define AP_SSID       "wifi.tree"
#define AP_SSID_SETUP "wifi.tree-setup"

// Must be called first — initialises NVS flash.
void nvs_init(void);

// Returns true and fills ssid/pass if credentials are saved.
bool wifi_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

// Saves credentials to NVS.
void wifi_save_credentials(const char *ssid, const char *pass);

// Starts WiFi in AP+STA mode, scans networks.
// Returns pointer to a static buffer containing <option> tags for every found network.
const char *wifi_start_setup(void);

// Starts WiFi in AP+STA mode and connects STA.
// NAT is enabled automatically when the uplink gets an IP.
void wifi_start_portal(const char *sta_ssid, const char *sta_pass);

// Returns AP IP in network byte order (for DNS server).
uint32_t wifi_ap_ip(void);

// True once STA has an IP address (i.e. internet is reachable via uplink).
bool wifi_has_uplink(void);
