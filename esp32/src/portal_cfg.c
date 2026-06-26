#include "portal_cfg.h"
#include <string.h>
#include "nvs.h"

#define NS "portal"

// Field metadata + built-in default, in display order. UTF-8 literals (emoji)
// are stored/served as-is and HTML-escaped at render time.
static const struct {
    portalcfg_field_t f;
    const char       *def;
} FIELDS[] = {
    {{"emoji",  "Header emoji",       16,  0}, "\xF0\x9F\x8C\xB3"},                 // 🌳
    {{"title",  "Title",              40,  0}, "wifi.tree"},
    {{"tagline","Tagline",            120, 0}, "community wifi \xC2\xB7 please be mindful, it's shared"},
    {{"banner", "Announcement banner",200, 1}, ""},
    {{"whead",  "Welcome heading",    80,  0}, "Welcome to the gathering \xF0\x9F\x8C\xBF"}, // 🌿
    {{"wtext",  "Welcome text",       400, 1}, "This is shared, free, community wifi. Enter a name and grow a leaf to get online."},
    {{"footer", "Footer",             200, 1}, "Shared, fair, bandwidth-limited.\nBe kind, keep it light."},
    {{"accent", "Accent colour",      8,   0}, "#2e7d32"},
};
#define NF ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))
#define MAXVAL 420   // >= largest field maxlen + slack

static char s_val[NF][MAXVAL];

static int idx_of(const char *key) {
    for (int i = 0; i < NF; i++)
        if (strcmp(FIELDS[i].f.key, key) == 0) return i;
    return -1;
}

void portalcfg_init(void) {
    nvs_handle_t h;
    bool have = nvs_open(NS, NVS_READONLY, &h) == ESP_OK;
    for (int i = 0; i < NF; i++) {
        size_t len = sizeof(s_val[i]);
        if (!have || nvs_get_str(h, FIELDS[i].f.key, s_val[i], &len) != ESP_OK)
            strlcpy(s_val[i], FIELDS[i].def, sizeof(s_val[i]));
    }
    if (have) nvs_close(h);
}

const char *portalcfg_get(const char *key) {
    int i = idx_of(key);
    return i < 0 ? "" : s_val[i];
}

void portalcfg_set(const char *key, const char *val) {
    int i = idx_of(key);
    if (i < 0) return;
    strlcpy(s_val[i], val, FIELDS[i].f.maxlen + 1);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, s_val[i]);
        nvs_commit(h);
        nvs_close(h);
    }
}

void portalcfg_reset(void) {
    nvs_handle_t h;
    bool rw = nvs_open(NS, NVS_READWRITE, &h) == ESP_OK;
    for (int i = 0; i < NF; i++) {
        strlcpy(s_val[i], FIELDS[i].def, sizeof(s_val[i]));
        if (rw) nvs_set_str(h, FIELDS[i].f.key, s_val[i]);
    }
    if (rw) { nvs_commit(h); nvs_close(h); }
}

int portalcfg_count(void) { return NF; }

const portalcfg_field_t *portalcfg_field(int i) {
    return (i >= 0 && i < NF) ? &FIELDS[i].f : NULL;
}
