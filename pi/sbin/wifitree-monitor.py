#!/usr/bin/env python3
import subprocess, re, time, os, sys, shutil

os.environ["PATH"] = os.environ.get("PATH", "") + ":/usr/sbin:/sbin:/usr/bin:/bin"

WLAN = "uap0"
LEASES = "/var/lib/misc/dnsmasq.leases"
POLL = 2.0


def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return ""


def parse_leases():
    out = {}
    try:
        with open(LEASES) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 4:
                    _, mac, ip, host = parts[0], parts[1], parts[2], parts[3]
                    out[mac.lower()] = (ip, host if host != "*" else "")
    except FileNotFoundError:
        pass
    return out


def parse_stations():
    text = run(["sudo", "iw", "dev", WLAN, "station", "dump"])
    stations = {}
    mac = None
    for line in text.splitlines():
        m = re.match(r"Station ([0-9a-f:]+) \(on", line)
        if m:
            mac = m.group(1).lower()
            stations[mac] = {}
            continue
        if mac is None:
            continue
        line = line.strip()
        if line.startswith("rx bytes:"):
            stations[mac]["rx_bytes"] = int(line.split(":")[1].strip())
        elif line.startswith("tx bytes:"):
            stations[mac]["tx_bytes"] = int(line.split(":")[1].strip())
        elif line.startswith("signal avg:"):
            m2 = re.search(r"(-?\d+)", line.split(":")[1])
            if m2:
                stations[mac]["signal"] = int(m2.group(1))
        elif line.startswith("signal:") and "signal" not in stations[mac]:
            m2 = re.search(r"(-?\d+)", line.split(":")[1])
            if m2:
                stations[mac]["signal"] = int(m2.group(1))
        elif line.startswith("connected time:"):
            stations[mac]["connected"] = line.split(":")[1].strip()
        elif line.startswith("tx bitrate:"):
            stations[mac]["tx_rate"] = line.split(":")[1].strip()
    return stations


def current_channel():
    text = run(["sudo", "iw", "dev", WLAN, "info"])
    m = re.search(r"channel (\d+)", text)
    return m.group(1) if m else "?"


def parse_cake(dev):
    text = run(["sudo", "tc", "-s", "qdisc", "show", "dev", dev])
    m = re.search(r"bandwidth (\S+)", text)
    bw = m.group(1) if m else "?"
    m2 = re.search(r"Sent (\d+) bytes (\d+) pkt \(dropped (\d+)", text)
    sent_bytes, sent_pkts, dropped = (m2.groups() if m2 else ("0", "0", "0"))
    m3 = re.search(r"backlog (\S+) (\d+)p", text)
    backlog = m3.group(1) if m3 else "0b"
    return {"bw": bw, "sent_bytes": int(sent_bytes), "dropped": int(dropped), "backlog": backlog}


def human(bps):
    for unit in ["bps", "Kbps", "Mbps", "Gbps"]:
        if bps < 1000:
            return f"{bps:.0f}{unit}"
        bps /= 1000
    return f"{bps:.1f}Tbps"


def humanb(b):
    for unit in ["B", "KB", "MB", "GB"]:
        if b < 1024:
            return f"{b:.0f}{unit}"
        b /= 1024
    return f"{b:.1f}TB"


def main():
    prev = {}
    prev_t = time.time()
    while True:
        leases = parse_leases()
        stations = parse_stations()
        cake_down = parse_cake(WLAN)
        cake_up = parse_cake("ifb0")
        now = time.time()
        dt = max(now - prev_t, 0.001)

        rows = []
        total_down_rate = 0
        total_up_rate = 0
        for mac, st in stations.items():
            ip, host = leases.get(mac, ("?", ""))
            rx = st.get("rx_bytes", 0)
            tx = st.get("tx_bytes", 0)
            p = prev.get(mac, {"rx_bytes": rx, "tx_bytes": tx})
            up_rate = max((rx - p.get("rx_bytes", rx)) * 8 / dt, 0)
            down_rate = max((tx - p.get("tx_bytes", tx)) * 8 / dt, 0)
            total_up_rate += up_rate
            total_down_rate += down_rate
            rows.append({
                "ip": ip, "host": host or "-", "mac": mac,
                "signal": st.get("signal", "-"),
                "down_rate": down_rate, "up_rate": up_rate,
                "down_total": tx, "up_total": rx,
                "tx_phy": st.get("tx_rate", "-"),
                "connected": st.get("connected", "-"),
            })
            prev[mac] = st

        cols = shutil.get_terminal_size((100, 20)).columns
        sys.stdout.write("\033[H\033[J")
        print("=" * min(cols, 100))
        print(f" wifi.tree  --  SSID: wifi.tree  channel {current_channel()}  --  {len(rows)} client(s) connected")
        print(f" shaper: down {cake_down['bw']} (sent {humanb(cake_down['sent_bytes'])}, dropped {cake_down['dropped']}, backlog {cake_down['backlog']})")
        print(f"         up   {cake_up['bw']} (sent {humanb(cake_up['sent_bytes'])}, dropped {cake_up['dropped']}, backlog {cake_up['backlog']})")
        print(f" aggregate now: down {human(total_down_rate)}  up {human(total_up_rate)}")
        print("-" * min(cols, 100))
        print(f" {'IP':<14}{'HOSTNAME':<16}{'MAC':<18}{'SIG':>5}{'DOWN':>10}{'UP':>10}{'TOTAL DN':>10}{'TOTAL UP':>10}")
        for r in sorted(rows, key=lambda x: -x["down_rate"]):
            print(f" {r['ip']:<14}{r['host'][:15]:<16}{r['mac']:<18}{str(r['signal']):>5}"
                  f"{human(r['down_rate']):>10}{human(r['up_rate']):>10}"
                  f"{humanb(r['down_total']):>10}{humanb(r['up_total']):>10}")
        if not rows:
            print(" (no clients connected)")
        print("=" * min(cols, 100))
        print(" Ctrl-C to exit. Refreshing every %.0fs..." % POLL)

        prev_t = now
        time.sleep(POLL)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
