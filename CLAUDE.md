# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Wifi Tree is a community captive-portal AP system. Visitors connect to the AP, enter a name to "grow a leaf", and get internet access. The leaf metaphor frames shared bandwidth as a shared resource (leaves expire after 3 hours, data runs out, be kind).

Two independent implementations share a common spec:
- **Pi** (`pi/`) — full-featured, Raspberry Pi reference implementation in Python
- **ESP32** (`esp32/`) — lightweight, portable implementation in C (ESP-IDF via PlatformIO)

The canonical behavior and visual spec lives in `spec/` and both implementations must conform to it.

## ESP32 Firmware

Built with PlatformIO using the ESP-IDF framework. Target board: `esp32dev` on `/dev/ttyUSB0`.

```bash
# Build
pio run

# Flash to device
pio run -t upload

# Serial monitor (115200 baud)
pio device monitor

# Build + flash + monitor in one step
pio run -t upload && pio device monitor
```

### ESP32 Architecture

`main.c` checks NVS for stored Wi-Fi credentials at boot and branches into one of two modes:

- **Setup mode** (no credentials): Broadcasts `wifi.tree-setup` AP, spoofs all DNS to self, serves a network-picker wizard at `/`. `POST /save` stores credentials and reboots.
- **Portal mode** (credentials found): Connects to uplink as STA, enables NAPT, broadcasts `wifi.tree` AP, spoofs only captive-portal probe domains to self (forwards everything else to 8.8.8.8), serves the leaf-growing portal at `/`.

Key source files:
- `src/main.c` — boot entry, mode decision
- `src/wifi_manager.c` — NVS credential storage, AP+STA setup, NAPT
- `src/dns_server.c` — selective DNS spoofing
- `src/http_server.c` — HTTP handlers for both modes
- `src/html.h` — all HTML templates as C string literals

`sdkconfig.defaults` enables lwIP NAPT (`CONFIG_LWIP_IP_NAPT=y`) which is required for internet forwarding.

## Pi Implementation

Python scripts run as systemd services. The shared library lives at `/usr/local/lib/wifitree/` (deployed from `pi/lib/`). Scripts are deployed to `/usr/local/sbin/`.

### Services and Scripts

| Script | Service | Purpose |
|---|---|---|
| `pi/sbin/wifitree-portal.py` | `wifitree-portal.service` | Captive portal HTTP server |
| `pi/sbin/wifitree-webadmin.py` | `wifitree-webadmin.service` | Admin panel on `:8090` (LAN-only) |
| `pi/sbin/wifitree-accountd.py` | `wifitree-accountd.service` | Bandwidth metering via nftables, polls every 30s |
| `pi/sbin/wifitree-monitor.py` | *(run manually)* | Live terminal dashboard of connected clients |
| `pi/sbin/wifitree-admin.py` | *(CLI tool)* | CLI user management (list, ban, reset-usage, etc.) |
| `pi/sbin/wifitree-shaping.sh` | `wifitree-shaping.service` | Sets up tc/CAKE qdiscs for 20Mbit pool shaping |
| `pi/sbin/wifitree-uap0-setup.sh` | `wifitree-uap0.service` | Creates the `uap0` virtual AP interface |

### Shared Python Libraries

- `pi/lib/wtdb.py` — SQLite database at `/var/lib/wifitree/wifitree.db`. All user state: MAC, name, first_seen, last_checkin, bytes_used_month, per-device bw/data caps. Settings (leaf TTL, default caps) are read from `/etc/wifitree/settings.json` on every call — no restart needed to change them.
- `pi/lib/wtconfig.py` — Portal appearance config from `/etc/wifitree/portal.json`. Also live-reloaded; covers emoji, title, tagline, banner, footer, welcome text, accent color.

### Network Setup (Pi)

- AP interface: `uap0` (virtual), subnet `10.42.0.0/24`, gateway `10.42.0.1`
- Uplink: `wlan0` (STA to home network)
- NAT via nftables (`inet wifitree` table)
- Traffic shaping via tc/CAKE on `uap0` (download) and `ifb0` (upload mirror)
- `wifitree-accountd` reads nftables meter sets (`upload_meter`, `download_meter`) and syncs per-IP byte counts to SQLite via MAC→IP from dnsmasq leases
- `wifitree-accountd` also syncs `full_speed_macs`/`full_speed_ips` nftables sets to enforce per-device caps and monthly quotas

### Pi Config Files

| Path | Purpose |
|---|---|
| `/etc/wifitree/portal.json` | Appearance strings (title, emoji, colors, copy) |
| `/etc/wifitree/settings.json` | Operational settings (leaf TTL, data caps, BW caps) |
| `/etc/wifitree/webadmin.conf` | Admin panel password hash |
| `/var/lib/wifitree/wifitree.db` | SQLite user/usage database |
| `/var/lib/misc/dnsmasq.leases` | dnsmasq DHCP leases (MAC→IP mapping) |

### Pi Service Management

```bash
# Deploy library
sudo cp pi/lib/*.py /usr/local/lib/wifitree/

# Restart a service after editing its script
sudo systemctl restart wifitree-portal

# Live monitor dashboard
sudo python3 /usr/local/sbin/wifitree-monitor.py

# CLI admin
sudo python3 /usr/local/sbin/wifitree-admin.py list
sudo python3 /usr/local/sbin/wifitree-admin.py --help
```

## Spec Compliance

Both implementations must conform to `spec/behavior.md` (network architecture, DNS rules, portal flow, visitor states) and `spec/design.md` (visual identity, copy strings, page structure, leaf animation). The canonical CSS is `spec/design.css`.

When adding features or changing behavior, update the spec first if the change is platform-agnostic, then update both implementations if applicable.
