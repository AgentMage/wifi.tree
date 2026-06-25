#include "wifi_manager.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/lwip_napt.h"
#include "lwip/inet.h"

#define TAG "wifi"

static esp_netif_t *s_ap  = NULL;
static esp_netif_t *s_sta = NULL;
static uint32_t     s_ap_ip;

static char s_networks_html[4096];

// ── NVS ──────────────────────────────────────────────────────────────────────

void nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
}

bool wifi_load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK) && (ssid[0] != '\0');
    if (ok) nvs_get_str(h, "pass", pass, &pass_len);
    nvs_close(h);
    return ok;
}

void wifi_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved credentials for: %s", ssid);
}

uint32_t wifi_ap_ip(void) {
    return s_ap_ip;
}

// ── Common init ───────────────────────────────────────────────────────────────

static void base_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap  = esp_netif_create_default_wifi_ap();
    s_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Set custom AP IP (10.42.0.1) before start so DHCP issues correct subnet
    s_ap_ip = esp_ip4addr_aton(AP_IP_STR);
    esp_netif_ip_info_t ip = {};
    ip.ip.addr      = s_ap_ip;
    ip.gw.addr      = s_ap_ip;
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap));
}

static void configure_ap(const char *ssid) {
    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = 6,
            .max_connection = 10,
            .authmode       = WIFI_AUTH_OPEN,
        }
    };
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

// ── Event handler (portal mode only) ─────────────────────────────────────────

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Uplink IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        // Re-enable NAT each time STA (re)connects
        ip_napt_enable(s_ap_ip, 1);
        ESP_LOGI(TAG, "NAT enabled");
    }
}

// ── Setup mode ────────────────────────────────────────────────────────────────

const char *wifi_start_setup(void) {
    base_init();
    configure_ap(AP_SSID_SETUP);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Scanning networks...");
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_wifi_scan_start(&sc, true); // blocking

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (recs) {
        esp_wifi_scan_get_ap_records(&n, recs);
        s_networks_html[0] = '\0';
        for (int i = 0; i < n; i++) {
            char opt[300];
            // Escape quotes in SSID to avoid breaking the HTML attribute
            char safe_ssid[64];
            const char *src = (char *)recs[i].ssid;
            char *dst = safe_ssid;
            while (*src && dst < safe_ssid + sizeof(safe_ssid) - 2) {
                if (*src == '"') { *dst++ = '\\'; }
                *dst++ = *src++;
            }
            *dst = '\0';
            snprintf(opt, sizeof(opt),
                     "<option value=\"%s\">%s (%d dBm)</option>",
                     safe_ssid, safe_ssid, recs[i].rssi);
            strlcat(s_networks_html, opt, sizeof(s_networks_html));
        }
        free(recs);
    }
    if (s_networks_html[0] == '\0')
        strlcpy(s_networks_html,
                "<option value=''>No networks found — reload page</option>",
                sizeof(s_networks_html));

    ESP_LOGI(TAG, "Setup AP up: %s  %s", AP_SSID_SETUP, AP_IP_STR);
    return s_networks_html;
}

// ── Portal mode ───────────────────────────────────────────────────────────────

void wifi_start_portal(const char *sta_ssid, const char *sta_pass) {
    base_init();

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,          on_wifi_event, NULL);

    configure_ap(AP_SSID);

    wifi_config_t sta_cfg = {};
    strlcpy((char *)sta_cfg.sta.ssid,     sta_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, sta_pass, sizeof(sta_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    // Enable NAT immediately — it will start routing once STA gets an IP
    ip_napt_enable(s_ap_ip, 1);
    ESP_LOGI(TAG, "Portal AP up: %s  %s  connecting to: %s",
             AP_SSID, AP_IP_STR, sta_ssid);
}
