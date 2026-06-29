# 🌳 wifi.tree

> *Off-grid, not out of touch.*

Community captive-portal wifi for festivals, wilderness gatherings, and anywhere
someone hauls a Starlink dish into a place that has no business having internet.
Visitors connect, type a name to **grow a leaf**, and get wood-speed access —
capped on purpose, shared fairly, free to all.

The software is **wifi.tree**. The person who plants it is a **wifi.mom**.
Pre-built hardware ships from [wifi.mom](https://wifi.mom).

---

## Products

| | **wifi.tree/sprout** | **wifi.tree/sapling** |
|---|---|---|
| **Hardware** | ESP32 DevKit V1 | Raspberry Pi Zero W (1 or 2) |
| **Cost** | ~$10 | ~$35+ |
| **Best for** | Parking lot, pop-up, hauling light | Base camp, long gathering, more visitors |
| **Setup** | Captive setup wizard on first boot | Manual config |
| **Docs** | [`esp32/`](esp32/) | [`pi/`](pi/) |

---

## How it works

1. **You bring the signal** — Starlink, hotspot, whatever you packed in
2. **You plant a tree** — plug in a sprout or sapling, point it at your uplink
3. **The camp grows leaves** — neighbors connect, type a name, get online
4. **Leaves fall** — after a few hours; grow another whenever

Speed is capped per-visitor by design. One dish, kept fair.

---

## Repository layout

```
spec/               Canonical spec — both implementations must conform
  behavior.md         Network architecture, DNS rules, portal flow
  design.md           Visual identity, copy strings, page structure
  design.css          Canonical stylesheet

esp32/              wifi.tree/sprout — ESP32, C (ESP-IDF via PlatformIO)
  src/                Firmware source
  platformio.ini
  sdkconfig.defaults

pi/                 wifi.tree/sapling — Raspberry Pi Zero, Python
  sbin/               Service scripts
  lib/                Shared Python library (wtdb, wtconfig)
  systemd/            Unit files
  hostapd/            AP config
  dnsmasq/            DNS/DHCP config
  config/             Default runtime config (portal.json, settings.json)

web/                wifi.mom website (static HTML/CSS, nginx Docker)
```

---

## Implementation comparison

| Feature | sprout (ESP32) | sapling (Pi) |
|---|---|---|
| Visitor tracking | NVS, per-MAC, survives reboot | SQLite, per-MAC |
| Returning-visitor status card | ✅ | ✅ |
| Internet gated by leaf | ✅ | ✅ |
| Leaf TTL (admin-configurable) | ✅ | ✅ |
| Bandwidth shaping | Token bucket, per-client kbps | tc/CAKE |
| Connected-time budget | ✅ lifetime cap | ✅ |
| Data budget | ✅ lifetime bytes | Monthly (calendar) |
| Per-visitor cap overrides | ✅ | ✅ |
| Admin panel | ✅ `/admin` on the AP | ✅ port 8090, LAN-side |
| Portal customization | ✅ `/admin/customize` | ✅ `portal.json` |
| Setup wizard | ✅ captive setup AP | Manual |
| HTTPS | ❌ | ✅ self-signed |

---

## License

wifi.tree is released under the
[PolyForm Noncommercial License](LICENSE) — free for personal, educational,
and community use. Selling hardware or services built on it requires a
commercial license: [lilly.fiorino@gmail.com](mailto:lilly.fiorino@gmail.com)

---

*Wood-speed, shared, fair. Leave no trace, leave a little love.*
