#pragma once
#include <stdbool.h>

// Operator-tunable settings, persisted in NVS namespace "cfg".

void config_init(void);

// Leaf freshness window. 0 => leaves never expire. Default 3h.
int  config_leaf_ttl_seconds(void);
void config_set_leaf_ttl_seconds(int seconds);

// Per-client bandwidth cap in kbit/s. 0 => uncapped. Default 100.
int  config_client_kbps(void);
void config_set_client_kbps(int kbps);

// Lifetime connected-time budget per visitor, in seconds. 0 => no limit.
// When a visitor's accumulated online time reaches this, they're cut off until
// an admin resets them. Default 0 (off).
int  config_connected_cap_seconds(void);
void config_set_connected_cap_seconds(int seconds);

// Lifetime data budget per visitor, in megabytes (up + down). 0 => no limit.
// When a visitor's accumulated traffic reaches this, they're cut off until an
// admin resets them. Default 0 (off).
int  config_data_cap_mb(void);
void config_set_data_cap_mb(int mb);

// Admin password (stored as salted SHA-256, never plaintext).
bool config_has_admin_password(void);
void config_set_admin_password(const char *pw);
bool config_check_admin_password(const char *pw);
