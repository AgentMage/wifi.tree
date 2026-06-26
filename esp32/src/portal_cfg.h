#pragma once

// Operator-customizable portal appearance, persisted in NVS namespace "portal".
// Mirrors the Pi's wtconfig.py / portal.json. Live — changes apply on the next
// page load, no reboot.

typedef struct {
    const char *key;     // NVS key + HTML form field name
    const char *label;   // human label on the customize page
    int         maxlen;  // max stored length
    int         multiline; // 1 => render as <textarea>
} portalcfg_field_t;

void portalcfg_init(void);

// Current value for a key (cached in RAM); "" if the key is unknown.
const char *portalcfg_get(const char *key);

// Update one key (truncated to its maxlen) and persist it.
void portalcfg_set(const char *key, const char *val);

// Restore every field to its built-in default.
void portalcfg_reset(void);

// Field table, for rendering/parsing the customize form.
int portalcfg_count(void);
const portalcfg_field_t *portalcfg_field(int i);
