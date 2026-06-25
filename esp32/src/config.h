#pragma once
#include <stdbool.h>

// Operator-tunable settings, persisted in NVS namespace "cfg".

void config_init(void);

// Leaf freshness window. 0 => leaves never expire. Default 3h.
int  config_leaf_ttl_seconds(void);
void config_set_leaf_ttl_seconds(int seconds);

// Admin password (stored as salted SHA-256, never plaintext).
bool config_has_admin_password(void);
void config_set_admin_password(const char *pw);
bool config_check_admin_password(const char *pw);
