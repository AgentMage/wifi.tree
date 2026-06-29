# 🌿 wifi.tree/sapling — Raspberry Pi Zero

Full-featured captive-portal wifi on a Raspberry Pi Zero W (1 or 2).
More visitor capacity, calendar-month data quotas, tc/CAKE traffic shaping,
and HTTPS. Best for base camps and longer gatherings.

---

## Services

| Service | Purpose |
|---|---|
| `wifitree-uap0` | Brings up the virtual AP interface (`uap0`) |
| `hostapd` | 802.11 AP on `uap0` |
| `wifitree-portal` | Captive portal HTTP/HTTPS (ports 80, 443, 8080, 8443) |
| `wifitree-accountd` | Expires leaves, meters data, resets monthly quotas |
| `wifitree-webadmin` | Operator admin panel (port 8090, LAN-side) |
| `wifitree-shaping` | tc/CAKE bandwidth shaping per client |

---

## File layout

| Repo path | Device path |
|---|---|
| `sbin/wifitree-portal.py` | `/usr/local/sbin/wifitree-portal.py` |
| `sbin/wifitree-accountd.py` | `/usr/local/sbin/wifitree-accountd.py` |
| `sbin/wifitree-webadmin.py` | `/usr/local/sbin/wifitree-webadmin.py` |
| `sbin/wifitree-monitor.py` | `/usr/local/sbin/wifitree-monitor.py` |
| `sbin/wifitree-admin.py` | `/usr/local/sbin/wifitree-admin.py` |
| `sbin/wifitree-shaping.sh` | `/usr/local/sbin/wifitree-shaping.sh` |
| `sbin/wifitree-uap0-setup.sh` | `/usr/local/sbin/wifitree-uap0-setup.sh` |
| `lib/wtdb.py` | `/usr/local/lib/wifitree/wtdb.py` |
| `lib/wtconfig.py` | `/usr/local/lib/wifitree/wtconfig.py` |
| `systemd/*.service` | `/etc/systemd/system/` |
| `hostapd/hostapd.conf` | `/etc/hostapd/hostapd.conf` |
| `dnsmasq/wifitree.conf` | `/etc/dnsmasq.d/wifitree.conf` |
| `config/portal.json` | `/etc/wifitree/portal.json` |
| `config/settings.json` | `/etc/wifitree/settings.json` |

---

## Network setup

| | |
|---|---|
| AP interface | `uap0` (virtual), `10.42.0.0/24`, gateway `10.42.0.1` |
| Uplink | `wlan0` (STA to home network / hotspot) |
| NAT | nftables (`inet wifitree` table) |
| Shaping | tc/CAKE on `uap0` (download) + `ifb0` (upload mirror) |

---

## Common commands

```sh
# Deploy library after editing
sudo cp pi/lib/*.py /usr/local/lib/wifitree/

# Restart a service after editing its script
sudo systemctl restart wifitree-portal

# Live terminal dashboard
sudo python3 /usr/local/sbin/wifitree-monitor.py

# CLI user management
sudo python3 /usr/local/sbin/wifitree-admin.py list
sudo python3 /usr/local/sbin/wifitree-admin.py --help
```

---

## Runtime data (not in repo)

| Path | Contents |
|---|---|
| `/var/lib/wifitree/wifitree.db` | SQLite — visitors, checkins, usage |
| `/etc/wifitree/portal.crt` / `portal.key` | TLS cert for HTTPS |
| `/etc/wifitree/webadmin.conf` | Hashed admin password |
| `/var/lib/misc/dnsmasq.leases` | DHCP leases (MAC→IP) |
