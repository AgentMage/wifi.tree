# Wifi Tree — Pi Implementation

Full-featured implementation running on the wavebug Raspberry Pi.

## Layout on device

| Repo path | Device path |
|---|---|
| `sbin/wifitree-portal.py` | `/usr/local/sbin/wifitree-portal.py` |
| `sbin/wifitree-accountd.py` | `/usr/local/sbin/wifitree-accountd.py` |
| `sbin/wifitree-webadmin.py` | `/usr/local/sbin/wifitree-webadmin.py` |
| `sbin/wifitree-monitor.py` | `/usr/local/sbin/wifitree-monitor.py` |
| `sbin/wifitree-shaping.sh` | `/usr/local/sbin/wifitree-shaping.sh` |
| `sbin/wifitree-uap0-setup.sh` | `/usr/local/sbin/wifitree-uap0-setup.sh` |
| `sbin/wifitree-admin.py` | `/usr/local/sbin/wifitree-admin.py` |
| `sbin/wavebug-projects.py` | `/usr/local/sbin/wavebug-projects.py` |
| `lib/wtdb.py` | `/usr/local/lib/wifitree/wtdb.py` |
| `lib/wtconfig.py` | `/usr/local/lib/wifitree/wtconfig.py` |
| `systemd/*.service` | `/etc/systemd/system/` |
| `hostapd/hostapd.conf` | `/etc/hostapd/hostapd.conf` |
| `hostapd/hostapd.service.override.conf` | `/etc/systemd/system/hostapd.service.d/override.conf` |
| `dnsmasq/wifitree.conf` | `/etc/dnsmasq.d/wifitree.conf` |
| `config/portal.json` | `/etc/wifitree/portal.json` |
| `config/settings.json` | `/etc/wifitree/settings.json` |

## Services

- **wifitree-uap0** — brings up the virtual AP interface (`uap0`)
- **hostapd** — 802.11 AP on `uap0`
- **wifitree-portal** — captive portal HTTP/HTTPS server (ports 80, 443, 8080, 8443)
- **wifitree-accountd** — background daemon: expires leaves, resets monthly data
- **wifitree-webadmin** — operator admin panel (port 8090)
- **wifitree-shaping** — `tc` bandwidth shaping per client
- **wavebug-projects** — LAN-side project toggle UI (port 8091)

## Runtime data (not in repo)

- `/var/lib/wifitree/wifitree.db` — SQLite: visitors, checkins, usage
- `/etc/wifitree/portal.crt` / `portal.key` — TLS cert for wifi.tree HTTPS
- `/etc/wavebug/admin.conf` — hashed admin password
