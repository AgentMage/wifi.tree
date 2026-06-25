#include "config.h"
#include <string.h>
#include "nvs.h"
#include "esp_random.h"
#include "psa/crypto.h"

#define NS               "cfg"
#define DEFAULT_TTL_SECS (3 * 3600)

static int s_ttl = DEFAULT_TTL_SECS;

void config_init(void) {
    psa_crypto_init();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "ttl", &v) == ESP_OK) s_ttl = v;
        nvs_close(h);
    }
}

int config_leaf_ttl_seconds(void) {
    return s_ttl;
}

void config_set_leaf_ttl_seconds(int seconds) {
    if (seconds < 0) seconds = 0;
    s_ttl = seconds;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "ttl", seconds);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ── Password (salted SHA-256) ─────────────────────────────────────────────────

static void to_hex(const unsigned char *in, int n, char *out) {
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

// digest = SHA-256(salt_hex || pw), hex-encoded into out[65].
static void hash_pw(const char *salt_hex, const char *pw, char out[65]) {
    unsigned char buf[160], digest[32];
    int n = snprintf((char *)buf, sizeof(buf), "%s%s", salt_hex, pw);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    size_t dlen = 0;
    psa_hash_compute(PSA_ALG_SHA_256, buf, n, digest, sizeof(digest), &dlen);
    to_hex(digest, 32, out);
}

bool config_has_admin_password(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = (nvs_get_str(h, "pw_hash", NULL, &len) == ESP_OK) && len > 1;
    nvs_close(h);
    return ok;
}

void config_set_admin_password(const char *pw) {
    unsigned char salt[16];
    esp_fill_random(salt, sizeof(salt));
    char salt_hex[33], hash_hex[65];
    to_hex(salt, sizeof(salt), salt_hex);
    hash_pw(salt_hex, pw, hash_hex);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "pw_salt", salt_hex);
        nvs_set_str(h, "pw_hash", hash_hex);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool config_check_admin_password(const char *pw) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    char salt_hex[33] = {0}, want[65] = {0};
    size_t sl = sizeof(salt_hex), wl = sizeof(want);
    bool have = nvs_get_str(h, "pw_salt", salt_hex, &sl) == ESP_OK &&
                nvs_get_str(h, "pw_hash", want, &wl) == ESP_OK;
    nvs_close(h);
    if (!have) return false;

    char got[65];
    hash_pw(salt_hex, pw, got);
    // Constant-time-ish compare.
    unsigned diff = 0;
    for (int i = 0; i < 64; i++) diff |= (unsigned)(got[i] ^ want[i]);
    return diff == 0;
}
