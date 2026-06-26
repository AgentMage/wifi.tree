# Wifi Tree — ESP32 Implementation

Minimal portable implementation targeting ESP32 DevKit V1 (~$5).  
Uses ESP-IDF (not Arduino) for lwIP NAT support.

## Build & flash

```sh
cd esp32/
pio run -t upload
```

Requires PlatformIO with the `espressif32` platform.  
Serial monitor at 115200 baud on `/dev/ttyUSB0`.

## First boot

1. Connect to `wifi.tree-setup`
2. Open any page — setup wizard appears
3. Pick uplink network + password → device reboots
4. Connect to `wifi.tree` — portal is live at `http://10.42.0.1/` or `http://wifi.tree/`

## Admin page

Browse to `http://wifi.tree/admin` from a device on the AP. On first run it
prompts you to set an admin password (you can also set it in the setup wizard);
after that it's password-protected. The dashboard shows uplink status, connected
visitors (name · hostname · leaf freshness), and lets you change the leaf TTL.

Hold the **BOOT button (GPIO0) for 5 s** to factory-reset (clears the saved
uplink credentials and reboots into setup mode). The onboard LED blinks fast in
setup mode, slow while connecting, and stays solid once the uplink is up.

## Differences from Pi implementation

| Feature | Pi | ESP32 |
|---|---|---|
| Per-user tracking | SQLite by MAC | RAM only, lost on reboot |
| Returning-visitor status card | Yes | Yes (name, hostname, freshness) |
| Internet gated by leaf | Yes | Yes (lwIP forward hook, on/off) |
| Leaf expiry | Yes | Yes (TTL, admin-configurable) |
| Data metering / quota | Yes | No |
| Bandwidth shaping | `tc` per client | No |
| Admin panel | Yes (port 8090, LAN-side) | Yes (`/admin`, on the AP) |
| HTTPS | Yes (self-signed) | No |
| Configurable copy/colors | Yes (portal.json) | Hardcoded to spec defaults |
