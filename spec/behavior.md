# Wifi Tree — Behavior Specification

## Concept

Wifi Tree is a community captive-portal AP. Visitors connect, see a portal
page, enter their name to "grow a leaf", and get internet access. The leaf
metaphor frames shared bandwidth as a shared resource: leaves dry up, data
runs out, be kind.

## Network Architecture

```
[client device]
      |
      | 802.11  (SSID: WifiTree, AP IP: 10.42.0.1)
      |
[Wifi Tree device]  ←→  upstream internet via uplink interface
      |
      | NAT (NAPT)
      |
[internet]
```

- AP subnet: `10.42.0.x/24`, gateway/portal `10.42.0.1`
- Uplink: separate interface (STA on ESP32, `uap0`/`wlan0` on Pi)
- NAT/NAPT bridges AP clients to the uplink

## DNS

In **portal mode**:
- Spoof the following domains → `10.42.0.1` to trigger OS captive-portal popups:
  - `wifi.tree` (our own hostname)
  - `captive.apple.com`, `www.apple.com`
  - `connectivitycheck.gstatic.com`, `connectivitycheck.android.com`, `clients3.google.com`
  - `www.msftconnecttest.com`, `www.msftncsi.com`, `ipv6.msftconnecttest.com`
  - `detectportal.firefox.com`, `nmcheck.gnome.org`
- All other DNS queries → forwarded to real resolver (`8.8.8.8`)

In **setup mode**: spoof everything (no uplink yet).

## Captive Portal HTTP

All HTTP requests to unknown paths → `302` redirect to `http://10.42.0.1/`.

### `GET /`
Render the welcome page (see [design.md](design.md)).

### `POST /`
Accept `name` field (max 40 chars). Register the visitor, show success screen.

## Setup Wizard

When no uplink credentials are stored:
1. Broadcast `WifiTree-Setup` AP
2. Serve a page listing nearby networks (scanned at boot)
3. User picks SSID + enters password → `POST /save`
4. Save credentials to persistent storage
5. Reboot into portal mode

## Visitor States (Pi full implementation)

| State | Condition | Portal shows |
|---|---|---|
| New | Never registered | Welcome card + name form |
| Active | Registered, leaf fresh, data ok | Status card + early-extend option |
| Expired | Leaf TTL elapsed | Expired card + extend form |
| Over quota | Monthly data cap hit | Over-quota card, no button |

The ESP32 implementation is single-session only (no persistent user tracking) —
everyone sees the new-visitor flow and gets a fresh leaf on each visit.

## Configurable Parameters

| Key | Default | Description |
|---|---|---|
| `title` | `Wifi Tree` | Page `<title>` and `<h1>` |
| `emoji` | `🌳` | Logo character |
| `tagline` | `community wifi · please be mindful, it's shared` | Subheading |
| `welcome_heading` | `Welcome to the gathering 🌿` | Heading on new-visitor card |
| `welcome_text` | *(see design.md)* | Body text on new-visitor card |
| `banner` | *(empty)* | Optional announcement shown on all portal views |
| `footer` | `Shared, fair, bandwidth-limited.\nBe kind, keep it light.` | Footer text |
| `accent` | `#2e7d32` | Primary brand color (hex) |
| `leaf_ttl_hours` | `3.0` | Hours a leaf stays fresh (0 = never expires) |
| `default_monthly_mb` | `100` | Monthly data cap per visitor (0 = unlimited) |
| `default_bw_mbit` | `0.1` | Per-client bandwidth cap in Mbit/s (0 = uncapped) |
