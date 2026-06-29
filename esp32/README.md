# 🌱 wifi.tree/sprout — ESP32

Portable captive-portal wifi on a $10 chip. Runs on any common ESP32 DevKit,
sips battery, sets up in minutes. The distribution target for
[wifi.mom](https://wifi.mom).

Built with ESP-IDF (not Arduino) for lwIP NAPT support.

---

## Quick start

```sh
# Build
pio run

# Flash to device (default: /dev/ttyUSB0)
pio run -t upload

# Serial monitor (115200 baud)
pio device monitor

# All in one
pio run -t upload && pio device monitor
```

---

## First boot — setup mode

1. Connect to **`wifi.tree-setup`**
2. Open any page — the setup wizard appears
3. Enter your uplink network + password → device reboots
4. Connect to **`wifi.tree`** — portal is live

> Hold **BOOT (GPIO0) for 5 s** to factory-reset and return to setup mode.

---

## LED status

| Pattern | State |
|---|---|
| Fast blink | Setup mode (no credentials) |
| Slow blink | Connecting to uplink |
| Solid | Online — portal active |

---

## Admin panel

Browse to **`http://wifi.tree/admin`** from a device on the AP.

On first run you'll set an admin password (or set it in the setup wizard).
From the dashboard you can:

- View connected visitors — name, hostname, leaf freshness, data used
- Adjust leaf TTL, per-client speed cap, connected-time budget, data budget
- Per-visitor: override speed cap, kick, ban, or reset usage
- Customize portal appearance at **`/admin/customize`** (emoji, title, tagline,
  banner, colors, welcome text, footer)

---

## Visitor persistence

Visitor records survive reboot. Each MAC address stores:

- Name, hostname
- Accumulated connected time and data (lifetime)
- Ban / budget override flags
- Per-visitor speed cap

Ephemeral fields (IP, leaf timer) are RAM-only and reset on reboot.

---

## Source layout

| File | Purpose |
|---|---|
| `src/main.c` | Boot, mode decision, LED task |
| `src/wifi_manager.c` | NVS credentials, AP+STA, NAPT |
| `src/http_server.c` | HTTP handlers (portal, setup, admin) |
| `src/client_state.c` | Per-MAC visitor table, NVS persistence |
| `src/shaper.c` | Per-client token-bucket bandwidth cap |
| `src/accounting.c` | 30 s task: fold bytes/time, enforce budgets |
| `src/dns_server.c` | Selective DNS spoofing |
| `src/config.c` | NVS-backed operational settings |
| `src/portal_cfg.c` | NVS-backed portal appearance |
| `src/reset_button.c` | GPIO0 5 s hold → factory reset |
| `src/led.c` | GPIO2 status LED |
| `src/html.h` | HTML templates as C string literals |
