# Wifi Tree

Community captive-portal wifi. Visitors connect, enter a name to grow a leaf,
and get internet access. Shared bandwidth, leaf metaphor, be kind.

## Repository layout

```
spec/           Canonical appearance & behavior spec (platform-agnostic)
  behavior.md     Network architecture, DNS rules, portal flow
  design.md       Visual identity, copy strings, page structure
  design.css      Canonical stylesheet both implementations derive from

pi/             Full implementation — Raspberry Pi (wavebug)
  sbin/           Service scripts
  lib/            Shared Python library (wtdb, wtconfig)
  systemd/        Unit files
  hostapd/        AP config
  dnsmasq/        DNS config
  config/         Default runtime config (portal.json, settings.json)

esp32/          Portable implementation — ESP32 DevKit (~$5/unit)
  src/            ESP-IDF C firmware
  platformio.ini
  sdkconfig.defaults
```

## Implementations

| | Pi | ESP32 |
|---|---|---|
| **Target** | Raspberry Pi (wavebug dev device) | ESP32 DevKit V1 |
| **Cost** | ~$40+ | ~$5 |
| **User tracking** | SQLite per-MAC | None |
| **Bandwidth shaping** | tc/htb | None |
| **Admin panel** | Yes | No |
| **HTTPS** | Yes | No |

The Pi is the development reference. The ESP32 is the distribution target.
