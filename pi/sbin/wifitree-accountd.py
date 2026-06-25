#!/usr/bin/env python3
import sys, subprocess, json, time, os

os.environ["PATH"] = os.environ.get("PATH", "") + ":/usr/sbin:/sbin:/usr/bin:/bin"
sys.path.insert(0, "/usr/local/lib/wifitree")
import wtdb

LEASES = "/var/lib/misc/dnsmasq.leases"
STATE_FILE = "/var/lib/wifitree/meter_state.json"
POLL = 30
NFT_TABLE = "inet wifitree"


def nft_json(*args):
    out = subprocess.run(["nft", "-j"] + list(args), capture_output=True, text=True).stdout
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return {}


def nft(*args):
    r = subprocess.run(["nft"] + list(args), capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"nft {' '.join(args)} failed: {r.stderr.strip()}\n")
    return r


def read_leases():
    mac_to_ip = {}
    ip_to_mac = {}
    try:
        with open(LEASES) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 3:
                    mac, ip = parts[1].lower(), parts[2]
                    mac_to_ip[mac] = ip
                    ip_to_mac[ip] = mac
    except FileNotFoundError:
        pass
    return mac_to_ip, ip_to_mac


def load_state():
    try:
        with open(STATE_FILE) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def save_state(state):
    os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
    with open(STATE_FILE, "w") as f:
        json.dump(state, f)


def meter_elements(name):
    data = nft_json("list", "set", "inet", "wifitree", name)
    result = {}
    for cmd in data.get("nftables", []):
        s = cmd.get("set")
        if not s or s.get("name") != name:
            continue
        for el in s.get("elem", []) or []:
            if isinstance(el, dict) and "elem" in el:
                e = el["elem"]
            else:
                e = el
            if isinstance(e, dict):
                val = e.get("val")
                counter = e.get("counter", {})
                bytes_ = counter.get("bytes", 0)
            else:
                val, bytes_ = e, 0
            if val:
                result[val] = bytes_
    return result


def sync_usage(ip_to_mac, state):
    up = meter_elements("upload_meter")
    down = meter_elements("download_meter")
    new_state = {}
    for ip, b in up.items():
        last = state.get("up_" + ip, 0)
        delta = b - last if b >= last else b
        new_state["up_" + ip] = b
        mac = ip_to_mac.get(ip)
        if mac and delta > 0:
            wtdb.add_bytes(mac, delta)
    for ip, b in down.items():
        last = state.get("down_" + ip, 0)
        delta = b - last if b >= last else b
        new_state["down_" + ip] = b
        mac = ip_to_mac.get(ip)
        if mac and delta > 0:
            wtdb.add_bytes(mac, delta)
    save_state(new_state)


def current_set_elements(name):
    data = nft_json("list", "set", "inet", "wifitree", name)
    vals = set()
    for cmd in data.get("nftables", []):
        s = cmd.get("set")
        if not s or s.get("name") != name:
            continue
        for el in s.get("elem", []) or []:
            if isinstance(el, dict) and "elem" in el:
                e = el["elem"]
                val = e.get("val") if isinstance(e, dict) else e
            else:
                val = el
            if val:
                vals.add(val)
    return vals


def sync_set(name, desired):
    current = current_set_elements(name)
    for val in desired - current:
        nft("add", "element", "inet", "wifitree", name, "{ " + val + " }")
    for val in current - desired:
        nft("delete", "element", "inet", "wifitree", name, "{ " + val + " }")


def sync_full_speed_sets(mac_to_ip):
    users = wtdb.list_users()
    full_macs, full_ips = set(), set()
    for u in users:
        if wtdb.is_full_speed(u):
            full_macs.add(u["mac"])
            ip = mac_to_ip.get(u["mac"])
            if ip:
                full_ips.add(ip)

    sync_set("full_speed_macs", full_macs)
    sync_set("full_speed_ips", full_ips)


def sync_custom_bw(mac_to_ip):
    users = wtdb.list_users()
    nft("flush", "chain", "inet", "wifitree", "custom_bw")
    for u in users:
        if not u["bw_limit_mbit"] or not wtdb.is_full_speed(u):
            continue
        ip = mac_to_ip.get(u["mac"])
        if not ip:
            continue
        kbytes = max(int(u["bw_limit_mbit"] * 125), 1)
        nft("add", "rule", "inet", "wifitree", "custom_bw",
            "oifname", "uap0", "ip", "daddr", ip,
            "limit", "rate", "over", f"{kbytes}", "kbytes/second", "drop")
        nft("add", "rule", "inet", "wifitree", "custom_bw",
            "iifname", "uap0", "ip", "saddr", ip,
            "limit", "rate", "over", f"{kbytes}", "kbytes/second", "drop")


def main():
    while True:
        try:
            mac_to_ip, ip_to_mac = read_leases()
            state = load_state()
            sync_usage(ip_to_mac, state)
            sync_full_speed_sets(mac_to_ip)
            sync_custom_bw(mac_to_ip)
        except Exception as e:
            sys.stderr.write(f"wifitree-accountd error: {e}\n")
        time.sleep(POLL)


if __name__ == "__main__":
    main()
