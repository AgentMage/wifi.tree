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

## Differences from Pi implementation

| Feature | Pi | ESP32 |
|---|---|---|
| Per-user tracking | SQLite by MAC | None (single-session) |
| Leaf expiry / data metering | Yes | No |
| Bandwidth shaping | `tc` per client | No |
| Admin panel | Yes (port 8090) | No |
| HTTPS | Yes (self-signed) | No |
| Configurable copy/colors | Yes (portal.json) | Hardcoded to spec defaults |
