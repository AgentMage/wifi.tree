#!/usr/bin/env python3
"""Wifi Tree web admin panel — LAN-side only, password protected.

Run with no args to serve. Run with --set-password to (re)set the admin
password interactively. Listens on :8090 on every interface but refuses any
request that arrives on the 10.42.0.x AP side, so gathering attendees on
`uap0` can never reach it — only devices on the home LAN (via wavebug.local).
"""
import sys, os, time, html, json, re, ssl, secrets, hashlib, threading, subprocess, datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
from http.cookies import SimpleCookie

os.environ["PATH"] = os.environ.get("PATH", "") + ":/usr/sbin:/sbin:/usr/bin:/bin"
sys.path.insert(0, "/usr/local/lib/wifitree")
import wtdb
import wtconfig

PORT = 8090
CONF = "/etc/wifitree/webadmin.conf"
LEASES = "/var/lib/misc/dnsmasq.leases"
AP_PREFIX = "10.42.0."
WLAN = "uap0"
SESSION_TTL = 12 * 3600
SAMPLE_EVERY = 2.0

SESSIONS = {}            # token -> expiry epoch
LATEST = {"clients": [], "agg_down": 0, "agg_up": 0, "channel": "?",
          "count": 0, "shaper_down": "?", "shaper_up": "?",
          "total_bytes": 0, "planted_at": 0, "ts": 0}


# ---------------------------------------------------------------- password ---
def hash_password(pw):
    salt = secrets.token_hex(16)
    h = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), 200000)
    return f"{salt}${h.hex()}"


def verify_password(pw):
    stored = load_conf().get("password_hash", "")
    if "$" not in stored:
        return False
    salt, want = stored.split("$", 1)
    try:
        h = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), 200000)
    except ValueError:
        return False
    return secrets.compare_digest(h.hex(), want)


def load_conf():
    d = {}
    try:
        with open(CONF) as f:
            for line in f:
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    k, v = line.split("=", 1)
                    d[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return d


def save_password(pw):
    os.makedirs(os.path.dirname(CONF), exist_ok=True)
    with open(CONF, "w") as f:
        f.write("# Wifi Tree web admin config\n")
        f.write(f"password_hash = {hash_password(pw)}\n")
    os.chmod(CONF, 0o600)


# ----------------------------------------------------------- live sampling ---
def run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return ""


def ap_is_running():
    return run(["systemctl", "is-active", "hostapd"]).strip() == "active"


def parse_leases():
    out = {}
    try:
        with open(LEASES) as f:
            for line in f:
                p = line.split()
                if len(p) >= 4:
                    out[p[1].lower()] = (p[2], p[3] if p[3] != "*" else "")
    except FileNotFoundError:
        pass
    return out


def parse_stations():
    text = run(["iw", "dev", WLAN, "station", "dump"])
    st, mac = {}, None
    for line in text.splitlines():
        m = re.match(r"Station ([0-9a-f:]+) \(on", line)
        if m:
            mac = m.group(1).lower(); st[mac] = {}; continue
        if mac is None:
            continue
        line = line.strip()
        if line.startswith("rx bytes:"):
            st[mac]["rx"] = int(line.split(":")[1].strip())
        elif line.startswith("tx bytes:"):
            st[mac]["tx"] = int(line.split(":")[1].strip())
        elif line.startswith("signal avg:"):
            m2 = re.search(r"(-?\d+)", line.split(":")[1])
            if m2:
                st[mac]["signal"] = int(m2.group(1))
        elif line.startswith("signal:") and "signal" not in st[mac]:
            m2 = re.search(r"(-?\d+)", line.split(":")[1])
            if m2:
                st[mac]["signal"] = int(m2.group(1))
    return st


def current_channel():
    m = re.search(r"channel (\d+)", run(["iw", "dev", WLAN, "info"]))
    return m.group(1) if m else "?"


def shaper_bw(dev):
    m = re.search(r"bandwidth (\S+)", run(["tc", "-s", "qdisc", "show", "dev", dev]))
    return m.group(1) if m else "?"


def sampler():
    prev, prev_t = {}, time.time()
    while True:
        try:
            leases = parse_leases()
            stations = parse_stations()
            now = time.time()
            dt = max(now - prev_t, 0.001)
            clients, agg_dn, agg_up = [], 0.0, 0.0
            for mac, s in stations.items():
                ip, host = leases.get(mac, ("?", ""))
                rx, tx = s.get("rx", 0), s.get("tx", 0)
                p = prev.get(mac, {"rx": rx, "tx": tx})
                up = max((rx - p.get("rx", rx)) * 8 / dt, 0)
                dn = max((tx - p.get("tx", tx)) * 8 / dt, 0)
                agg_dn += dn; agg_up += up
                u = wtdb.get_user(mac)
                clients.append({
                    "ip": ip, "host": host or "-", "mac": mac,
                    "name": (u["name"] if u else "") or "",
                    "signal": s.get("signal", "-"),
                    "down": dn, "up": up,
                    "down_total": tx, "up_total": rx,
                })
                prev[mac] = s
            prev_t = now
            clients.sort(key=lambda c: -c["down"])
            st = wtdb.get_stats()
            LATEST.update({"clients": clients, "agg_down": agg_dn, "agg_up": agg_up,
                           "channel": current_channel(), "count": len(clients),
                           "shaper_down": shaper_bw(WLAN), "shaper_up": shaper_bw("ifb0"),
                           "total_bytes": st["total_bytes"], "planted_at": st["planted_at"],
                           "ts": now})
        except Exception as e:
            sys.stderr.write(f"sampler error: {e}\n")
        time.sleep(SAMPLE_EVERY)


# ----------------------------------------------------------------- helpers ---
def human_bps(bps):
    for u in ["bps", "Kbps", "Mbps", "Gbps"]:
        if bps < 1000:
            return f"{bps:.0f} {u}"
        bps /= 1000
    return f"{bps:.1f} Tbps"


def human_b(b):
    for u in ["B", "KB", "MB", "GB"]:
        if b < 1024:
            return f"{b:.0f} {u}"
        b /= 1024
    return f"{b:.1f} TB"


def fmt_ago(ts):
    if not ts:
        return "never"
    s = time.time() - ts
    if s < 60:
        return f"{int(s)}s ago"
    if s < 3600:
        return f"{int(s/60)}m ago"
    if s < 86400:
        return f"{s/3600:.1f}h ago"
    return f"{s/86400:.1f}d ago"


def user_state(u):
    if u.get("force_full_speed"):
        return ("forced", "FORCED full")
    used = (u["bytes_used_month"] or 0) / 1024 / 1024
    quota = u["monthly_limit_mb"]
    if quota is None:
        quota = wtdb.default_monthly_mb()
    if quota and quota > 0 and used >= quota:
        return ("bad", "Over quota")
    ttl = wtdb.checkin_ttl_seconds()
    if ttl is not None and (time.time() - (u["last_checkin"] or 0)) > ttl:
        return ("warn", "Leaf dried up")
    return ("ok", "Online")


# -------------------------------------------------------------------- HTML ---
CSS = """
*{box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
 background:#0b1a0f;color:#eaffea;margin:0}
header{background:#12251a;border-bottom:1px solid #1f3d29;padding:14px 20px;
 display:flex;align-items:center;justify-content:space-between}
header h1{margin:0;font-size:1.3em;color:#9fe89f}
header a{color:#9fe89f;text-decoration:none;font-size:.9em;opacity:.8}
.wrap{max-width:1000px;margin:0 auto;padding:20px}
.cards{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:20px}
.stat{background:#12251a;border:1px solid #1f3d29;border-radius:12px;
 padding:14px 18px;flex:1;min-width:130px}
.stat .n{font-size:1.6em;font-weight:700;color:#9fe89f}
.stat .l{font-size:.8em;opacity:.7;margin-top:2px}
h2{color:#9fe89f;font-size:1.05em;border-bottom:1px solid #1f3d29;
 padding-bottom:6px;margin:26px 0 10px}
table{width:100%;border-collapse:collapse;font-size:.9em}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #1a2f20;vertical-align:middle}
th{font-size:.78em;text-transform:uppercase;opacity:.6;font-weight:600}
tr:hover td{background:#101f15}
.pill{display:inline-block;padding:2px 9px;border-radius:99px;font-size:.78em;font-weight:600}
.pill.ok{background:#1b3a1f;color:#8ef08e}
.pill.warn{background:#3a2f15;color:#ffd54f}
.pill.bad{background:#3a1b1b;color:#ff8a80}
.pill.forced{background:#16304a;color:#8ec9ff}
.bar{height:8px;background:#0b1a0f;border:1px solid #1f3d29;border-radius:99px;
 overflow:hidden;width:90px;display:inline-block;vertical-align:middle}
.bar .f{display:block;height:100%;border-radius:99px;min-width:2px}
form.inline{display:inline;margin:0}
input[type=number],input[type=text]{background:#0b1a0f;border:1px solid #2e7d32;
 color:#eaffea;border-radius:7px;padding:5px 7px;width:74px;font-size:.9em}
button{background:#2e7d32;color:#fff;border:none;border-radius:7px;
 padding:5px 10px;font-size:.85em;cursor:pointer;margin:1px}
button.sec{background:#33503a}
button.amber{background:#b8860b}
button.danger{background:#a33}
.muted{opacity:.55;font-size:.85em}
.login{max-width:340px;margin:9vh auto;background:#12251a;border:1px solid #1f3d29;
 border-radius:16px;padding:28px}
.login input{width:100%;padding:13px;font-size:1.1em;margin-bottom:12px}
.login button{width:100%;padding:14px;font-size:1.1em}
.login h1{color:#9fe89f;text-align:center}
.err{color:#ff8a80;text-align:center;margin-bottom:10px}
.ok-msg{color:#8ef08e;text-align:center;margin-bottom:10px}
.actions{white-space:nowrap}
.cust label{display:block;margin:16px 0 5px;font-size:.82em;opacity:.75;
 text-transform:uppercase;letter-spacing:.03em}
.cust input[type=text],.cust textarea{width:100%;padding:11px;border-radius:9px;
 border:1px solid #2e7d32;background:#0b1a0f;color:#eaffea;font-size:1em;font-family:inherit}
.cust textarea{min-height:58px;resize:vertical}
.cust input[type=color]{width:54px;height:38px;padding:2px;border-radius:8px;
 border:1px solid #2e7d32;background:#0b1a0f;vertical-align:middle}
.setrow{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin:6px 0 2px}
.cust .setrow input[type=number]{width:96px;margin:0;padding:9px;font-size:1em}
.cust .toggle{display:inline-flex;align-items:center;gap:7px;margin:0;
 text-transform:none;letter-spacing:0;opacity:1;font-size:.95em;cursor:pointer}
.cust .toggle input[type=checkbox]{width:18px;height:18px;margin:0;accent-color:#2e7d32}
.setrow .unit{opacity:.7;font-size:.92em}
.emoji-pick{margin-top:8px;display:flex;flex-wrap:wrap;gap:4px}
.emoji-pick button{font-size:1.5em;width:auto;padding:5px 8px;background:#18301d;margin:0}
.emoji-pick button:hover{background:#234a2b}
.preview{background:#0b1a0f;border:1px solid #1f3d29;border-radius:14px;
 padding:20px;text-align:center;margin-bottom:18px}
.preview .pe{font-size:3em;line-height:1}
.preview .pt{font-size:1.5em;font-weight:700;margin:2px 0}
.preview .pg{opacity:.7;font-size:.9em}
.preview .pb{background:#2a2410;border:1px solid #b8860b;color:#ffe9a8;
 border-radius:10px;padding:9px 12px;margin-top:12px;font-weight:600;white-space:pre-wrap}
"""


def page(title, body):
    return f"""<!DOCTYPE html><html><head><title>{title}</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>{CSS}</style></head><body>{body}</body></html>"""


def login_page(err=""):
    e = f'<p class="err">{html.escape(err)}</p>' if err else ""
    return page("Wifi Tree Admin", f"""
<div class="login">
  <h1>🌳 Wifi Tree</h1>
  <p class="muted" style="text-align:center">admin panel</p>
  {e}
  <form method="POST" action="/login">
    <input type="password" name="password" placeholder="admin password" autofocus required>
    <button type="submit">Log in</button>
  </form>
</div>""")


def change_password_page(err="", ok=""):
    e = f'<p class="err">{html.escape(err)}</p>' if err else ""
    o = f'<p class="ok-msg">{html.escape(ok)}</p>' if ok else ""
    return page("Wifi Tree Admin — change password", f"""
<header>
  <h1>🌳 Wifi Tree — admin</h1>
  <a href="/">&larr; back to dashboard</a>
</header>
<div class="login" style="margin-top:6vh">
  <h1 style="font-size:1.1em">Change admin password</h1>
  {e}{o}
  <form method="POST" action="/change-password">
    <input type="password" name="current" placeholder="current password" autofocus required>
    <input type="password" name="new1" placeholder="new password" required>
    <input type="password" name="new2" placeholder="repeat new password" required>
    <button type="submit">Change password</button>
  </form>
  <p class="muted">Changing it logs out every other device currently signed in
  (not this one).</p>
</div>""")


FESTIVAL_EMOJI = ["🌳", "🌲", "🌿", "🍄", "🔥", "🌈", "⛺", "🏕️", "🪕", "🎶",
                  "🎸", "☮️", "✨", "🌻", "🦋", "🌙", "⭐", "🌞", "💚", "🕉️",
                  "🧘", "🦌", "🐚", "🌸", "🍂", "🌀", "🎪", "🪩"]


def customize_page(cfg, ok=""):
    o = f'<p class="ok-msg">{html.escape(ok)}</p>' if ok else ""
    e = html.escape
    picks = "".join(
        f'<button type="button" onclick="setEmoji(this.textContent)">{em}</button>'
        for em in FESTIVAL_EMOJI)
    return page("Wifi Tree Admin — customize portal", f"""
<header>
  <h1>🌳 Wifi Tree — admin</h1>
  <a href="/">&larr; back to dashboard</a>
</header>
<div class="wrap" style="max-width:560px">
  <h2>Customize the attendee portal</h2>
  {o}
  <div class="preview" id="prev">
    <div class="pe" id="p-emoji">{e(cfg['emoji'])}</div>
    <div class="pt" id="p-title" style="color:{cfg['accent']}">{e(cfg['title'])}</div>
    <div class="pg" id="p-tag">{e(cfg['tagline'])}</div>
    <div class="pb" id="p-banner" style="{'' if cfg['banner'].strip() else 'display:none'}">{e(cfg['banner'])}</div>
  </div>

  <form class="cust" method="POST" action="/customize">
    <label>Header emoji</label>
    <input type="text" id="emoji" name="emoji" value="{e(cfg['emoji'])}"
           maxlength="16" oninput="upd()">
    <div class="emoji-pick">{picks}</div>

    <label>Title</label>
    <input type="text" name="title" value="{e(cfg['title'])}" maxlength="40" oninput="upd()">

    <label>Tagline (small line under the title)</label>
    <input type="text" name="tagline" value="{e(cfg['tagline'])}" maxlength="120" oninput="upd()">

    <label>Announcement banner (leave empty to hide it)</label>
    <textarea name="banner" maxlength="200" oninput="upd()"
      placeholder="e.g. Main circle at sunset 🔥  ·  Kitchen needs firewood">{e(cfg['banner'])}</textarea>

    <label>Welcome heading (first-time visitors)</label>
    <input type="text" name="welcome_heading" value="{e(cfg['welcome_heading'])}" maxlength="80">

    <label>Welcome text</label>
    <textarea name="welcome_text" maxlength="400">{e(cfg['welcome_text'])}</textarea>

    <label>Footer line</label>
    <textarea name="footer" maxlength="200">{e(cfg['footer'])}</textarea>

    <label>Accent colour (buttons)</label>
    <input type="color" name="accent" value="{cfg['accent']}" oninput="upd()">

    <div style="margin-top:22px">
      <button type="submit">Save — go live</button>
    </div>
  </form>

  <form method="POST" action="/customize" style="margin-top:10px"
        onsubmit="return confirm('Reset all portal text/emoji to defaults?')">
    <input type="hidden" name="action" value="reset">
    <button class="sec" type="submit" style="width:auto">Reset to defaults</button>
  </form>
  <p class="muted">Changes go live on the captive portal immediately — no
  restart, no reconnect needed.</p>
</div>
<script>
function g(n){{return document.querySelector('[name='+n+']')}}
function setEmoji(em){{document.getElementById('emoji').value=em;upd();}}
function upd(){{
  document.getElementById('p-emoji').textContent=document.getElementById('emoji').value;
  document.getElementById('p-title').textContent=g('title').value;
  document.getElementById('p-title').style.color=g('accent').value;
  document.getElementById('p-tag').textContent=g('tagline').value;
  var b=g('banner').value, pb=document.getElementById('p-banner');
  pb.textContent=b; pb.style.display=b.trim()?'block':'none';
}}
</script>""")


def settings_summary():
    ttl = wtdb.checkin_ttl_seconds()
    leaf = "never expires" if ttl is None else f"{wtdb.leaf_ttl_hours():g}h"
    mb = wtdb.default_monthly_mb()
    cap = "unlimited" if not mb or mb <= 0 else f"{mb:g} MB/mo"
    bw = wtdb.default_bw_mbit()
    speed = "uncapped" if not bw or bw <= 0 else f"{bw*1000:g} kbps"
    return f"Leaf life: <b>{leaf}</b> · default data cap: <b>{cap}</b> · default speed: <b>{speed}</b>"


def settings_page(ok=""):
    o = f'<p class="ok-msg">{html.escape(ok)}</p>' if ok else ""
    leaf_h = wtdb.leaf_ttl_hours()
    leaf_on = leaf_h > 0
    mb = wtdb.default_monthly_mb()
    mb_on = bool(mb and mb > 0)
    bw = wtdb.default_bw_mbit()           # stored internally in mbit
    bw_on = bool(bw and bw > 0)
    ap_on = ap_is_running()
    # When a knob is off, show the factory default in the (greyed) box so
    # re-enabling has a sensible starting value. Speed is shown in kbps.
    leaf_val = f"{leaf_h:g}" if leaf_on else f"{wtdb.DEFAULT_LEAF_TTL_HOURS:g}"
    mb_val = f"{mb:g}" if mb_on else f"{float(wtdb.DEFAULT_MONTHLY_MB):g}"
    bw_kbps = (bw if bw_on else wtdb.DEFAULT_BW_MBIT) * 1000
    bw_val = f"{bw_kbps:g}"
    ck = lambda on: "checked" if on else ""
    dis = lambda on: "" if on else "disabled"
    return page("Wifi Tree Admin — global settings", f"""
<header>
  <h1>🌳 Wifi Tree — admin</h1>
  <a href="/">&larr; back to dashboard</a>
</header>
<div class="wrap" style="max-width:560px">
  <h2>Global settings</h2>
  {o}
  <p class="muted">These apply network-wide. Leaf life takes effect within
  ~30s; the data &amp; speed defaults apply to devices the next time they
  register (existing accounts keep their current values unless you change them
  per-device on the dashboard).</p>

  <form class="cust" method="POST" action="/settings">

    <label>Leaf life — how long a check-in lasts</label>
    <div class="setrow">
      <label class="toggle"><input type="checkbox" name="leaf_on" {ck(leaf_on)}
        onchange="syncrow(this,'leaf')"> leaves expire</label>
      <input type="number" step="0.5" min="0.5" id="leaf" name="leaf_ttl_hours"
             value="{leaf_val}" {dis(leaf_on)}>
      <span class="unit">hours</span>
    </div>
    <p class="muted">Unchecked = leaves <b>never expire</b> (people register
    once and stay online, subject to the data cap).</p>

    <label>Default data cap per device, each month</label>
    <div class="setrow">
      <label class="toggle"><input type="checkbox" name="mb_on" {ck(mb_on)}
        onchange="syncrow(this,'mb')"> limit data</label>
      <input type="number" step="1" min="1" id="mb" name="default_monthly_mb"
             value="{mb_val}" {dis(mb_on)}>
      <span class="unit">MB</span>
    </div>
    <p class="muted">Unchecked = <b>unlimited data</b> for new registrations.</p>

    <label>Default speed cap per device</label>
    <div class="setrow">
      <label class="toggle"><input type="checkbox" name="bw_on" {ck(bw_on)}
        onchange="syncrow(this,'bw')"> cap speed</label>
      <input type="number" step="1" min="10" id="bw" name="default_bw_kbps"
             value="{bw_val}" {dis(bw_on)}>
      <span class="unit">kbps</span>
    </div>
    <p class="muted">Unchecked = <b>uncapped</b> per device (they share the
    AP-wide pool, ~20 Mbit). 100 kbps is the gentle default.</p>

    <label>AP — broadcast the "Wifi Tree" network</label>
    <div class="setrow">
      <label class="toggle"><input type="checkbox" name="ap_on" {ck(ap_on)}
        onchange="this.form.querySelector('#ap-warn').style.display=this.checked?'none':'block'">
        AP on</label>
    </div>
    <p class="muted">Uncheck to shut down the "Wifi Tree" network immediately —
    all connected devices will lose access.</p>
    <p id="ap-warn" class="muted" style="color:#ffd54f;{'' if not ap_on else 'display:none'}">
    ⚠ AP is currently off — save to turn it back on.</p>

    <div style="margin-top:22px"><button type="submit">Save settings</button></div>
  </form>
  <form method="POST" action="/settings" style="margin-top:10px"
        onsubmit="return confirm('Reset leaf/data/speed settings to factory defaults?')">
    <input type="hidden" name="action" value="reset">
    <button class="sec" type="submit" style="width:auto">Reset to defaults</button>
  </form>
</div>
<script>
function syncrow(cb,id){{document.getElementById(id).disabled=!cb.checked;}}
</script>""")


def bar(pct, color):
    return f'<span class="bar"><span class="f" style="width:{pct}%;background:{color}"></span></span>'


def dashboard():
    users = wtdb.list_users()
    st = wtdb.get_stats()
    planted = datetime.datetime.fromtimestamp(st["planted_at"]).strftime("%b %-d, %Y")
    rows = ""
    for u in users:
        cls, label = user_state(u)
        used = (u["bytes_used_month"] or 0) / 1024 / 1024
        quota = u["monthly_limit_mb"]
        if quota is None:
            quota = wtdb.default_monthly_mb()
        if not quota or quota <= 0:
            data_cell = f"{used:.0f} MB<br><span class='muted'>∞ unlimited</span>"
        else:
            dpct = min(100, int(used / quota * 100))
            dcolor = "#2e7d32" if dpct < 75 else ("#b8860b" if dpct < 100 else "#a33")
            data_cell = f"{used:.0f}/{quota:.0f} MB<br>{bar(dpct, dcolor)}"
        bw = f'{u["bw_limit_mbit"]*1000:g} kbps' if u["bw_limit_mbit"] else "—"
        mac = html.escape(u["mac"])
        name = html.escape(u["name"] or "")
        forced = u.get("force_full_speed")
        force_btn = (f'<button class="sec" name="action" value="unforce">un-force</button>'
                     if forced else
                     f'<button class="amber" name="action" value="force">force on</button>')
        rows += f"""
        <tr>
          <td>
            <form class="inline" method="POST" action="/action">
              <input type="hidden" name="mac" value="{mac}">
              <input type="text" name="name" value="{name}" placeholder="(no name)"
                     maxlength="40" style="width:120px">
              <button name="action" value="rename" title="save name">save</button>
            </form>
            <div class="muted">{mac}</div>
          </td>
          <td><span class="pill {cls}">{label}</span></td>
          <td>{data_cell}</td>
          <td>{bw}</td>
          <td class="muted">{fmt_ago(u['last_checkin'])}</td>
          <td class="actions">
            <form class="inline" method="POST" action="/action">
              <input type="hidden" name="mac" value="{mac}">
              <input type="number" step="1" min="0" name="bw" placeholder="kbps">
              <button name="action" value="set-bw">set bw</button>
              <button class="sec" name="action" value="clear-bw">clear</button>
              <input type="number" step="1" min="0" name="limit" placeholder="MB">
              <button name="action" value="set-limit">set cap</button>
              <button class="sec" name="action" value="reset">reset usage</button>
              {force_btn}
              <button class="danger" name="action" value="remove"
                onclick="return confirm('Remove {name or mac} entirely?')">remove</button>
            </form>
          </td>
        </tr>"""
    if not users:
        rows = '<tr><td colspan="6" class="muted">No registered devices yet.</td></tr>'

    return page("Wifi Tree Admin", f"""
<header>
  <h1>🌳 Wifi Tree — admin</h1>
  <span><a href="/settings">settings</a> &nbsp;·&nbsp;
  <a href="/customize">customize portal</a> &nbsp;·&nbsp;
  <a href="/change-password">change password</a> &nbsp;·&nbsp;
  <a href="/logout">log out</a></span>
</header>
<div class="wrap">
  <div class="cards" id="cards">
    <div class="stat"><div class="n" id="c-count">–</div><div class="l">connected now</div></div>
    <div class="stat"><div class="n" id="c-down">–</div><div class="l">download now</div></div>
    <div class="stat"><div class="n" id="c-up">–</div><div class="l">upload now</div></div>
    <div class="stat"><div class="n" id="c-ch">–</div><div class="l">channel</div></div>
    <div class="stat"><div class="n">{len(users)}</div><div class="l">registered total</div></div>
    <div class="stat"><div class="n" id="c-total">{human_b(st['total_bytes'])}</div><div class="l">used since planted</div></div>
  </div>
  <p class="muted" style="margin-top:-8px">🌱 Wifi Tree was planted on <b>{planted}</b> and has served
  <b id="c-total2">{human_b(st['total_bytes'])}</b> of data to the gathering since.</p>

  <h2>Connected devices <span class="muted" id="live-age"></span></h2>
  <table>
    <thead><tr><th>IP / host</th><th>name</th><th>signal</th>
      <th>down</th><th>up</th><th>total dn</th><th>total up</th></tr></thead>
    <tbody id="live"><tr><td colspan="7" class="muted">loading…</td></tr></tbody>
  </table>

  <h2>Registered accounts</h2>
  <table>
    <thead><tr><th>device</th><th>status</th><th>data this month</th><th>bw cap</th>
      <th>last leaf</th><th>actions</th></tr></thead>
    <tbody>{rows}</tbody>
  </table>
  <p class="muted">{settings_summary()} · "force on" ignores quota &amp;
  leaf-expiry · <a href="/settings">change global settings</a></p>
</div>
<script>
function hbps(b){{var u=['bps','Kbps','Mbps','Gbps'];var i=0;while(b>=1000&&i<3){{b/=1000;i++}}return b.toFixed(i?1:0)+' '+u[i]}}
function hb(b){{var u=['B','KB','MB','GB'];var i=0;while(b>=1024&&i<3){{b/=1024;i++}}return b.toFixed(i?1:0)+' '+u[i]}}
async function tick(){{
  try{{
    let r=await fetch('/api/monitor');let d=await r.json();
    document.getElementById('c-count').textContent=d.count;
    document.getElementById('c-down').textContent=hbps(d.agg_down);
    document.getElementById('c-up').textContent=hbps(d.agg_up);
    document.getElementById('c-ch').textContent=d.channel;
    if(d.total_bytes!=null){{document.getElementById('c-total').textContent=hb(d.total_bytes);
      var t2=document.getElementById('c-total2');if(t2)t2.textContent=hb(d.total_bytes);}}
    let tb=document.getElementById('live');
    if(!d.clients.length){{tb.innerHTML='<tr><td colspan=7 class=muted>(no devices connected)</td></tr>';}}
    else{{tb.innerHTML=d.clients.map(c=>`<tr><td>${{c.ip}}<br><span class=muted>${{c.host}}</span></td>
      <td>${{c.name||'<span class=muted>—</span>'}}</td><td>${{c.signal}} dBm</td>
      <td>${{hbps(c.down)}}</td><td>${{hbps(c.up)}}</td>
      <td>${{hb(c.down_total)}}</td><td>${{hb(c.up_total)}}</td></tr>`).join('');}}
    document.getElementById('live-age').textContent='(live, every 2s)';
  }}catch(e){{document.getElementById('live-age').textContent='(disconnected)';}}
}}
tick();setInterval(tick,2000);
</script>""")


# ----------------------------------------------------------------- handler ---
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    # --- helpers ---
    def _on_ap_side(self):
        try:
            return self.connection.getsockname()[0].startswith(AP_PREFIX)
        except Exception:
            return False

    def _token(self):
        c = SimpleCookie(self.headers.get("Cookie", ""))
        return c["wt_session"].value if "wt_session" in c else None

    def _authed(self):
        t = self._token()
        exp = SESSIONS.get(t)
        if exp and exp > time.time():
            return True
        if t in SESSIONS:
            del SESSIONS[t]
        return False

    def _send(self, body, code=200, ctype="text/html; charset=utf-8", headers=None):
        body = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or []):
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, loc, headers=None):
        self.send_response(303)
        self.send_header("Location", loc)
        for k, v in (headers or []):
            self.send_header(k, v)
        self.end_headers()

    # --- routing ---
    def do_GET(self):
        if self._on_ap_side():
            return self._send("Not available here.", 403, "text/plain")
        path = urlparse(self.path).path
        if path == "/logout":
            t = self._token()
            SESSIONS.pop(t, None)
            return self._redirect("/", [("Set-Cookie", "wt_session=; Max-Age=0; Path=/")])
        if not self._authed():
            return self._send(login_page())
        if path == "/api/monitor":
            return self._send(json.dumps(LATEST), ctype="application/json")
        if path == "/change-password":
            return self._send(change_password_page())
        if path == "/customize":
            return self._send(customize_page(wtconfig.load()))
        if path == "/settings":
            return self._send(settings_page())
        return self._send(dashboard())

    def do_POST(self):
        if self._on_ap_side():
            return self._send("Not available here.", 403, "text/plain")
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        data = parse_qs(self.rfile.read(length).decode())

        if path == "/login":
            pw = data.get("password", [""])[0]
            if verify_password(pw):
                tok = secrets.token_urlsafe(32)
                SESSIONS[tok] = time.time() + SESSION_TTL
                return self._redirect("/", [
                    ("Set-Cookie",
                     f"wt_session={tok}; HttpOnly; SameSite=Strict; Path=/; Max-Age={SESSION_TTL}")])
            return self._send(login_page("Wrong password."))

        if not self._authed():
            return self._redirect("/")

        if path == "/action":
            self._do_action(data)
            return self._redirect("/")

        if path == "/change-password":
            return self._do_change_password(data)

        if path == "/customize":
            return self._do_customize(data)

        if path == "/settings":
            return self._do_settings(data)

        return self._send("not found", 404, "text/plain")

    def _do_settings(self, data):
        if data.get("action", [""])[0] == "reset":
            wtdb.save_settings({
                "leaf_ttl_hours": wtdb.DEFAULT_LEAF_TTL_HOURS,
                "default_monthly_mb": float(wtdb.DEFAULT_MONTHLY_MB),
                "default_bw_mbit": wtdb.DEFAULT_BW_MBIT,
            })
            return self._send(settings_page(ok="Reset to factory defaults."))
        updates = {}
        # A checkbox absent from the POST means it was unchecked => that knob
        # is turned off (stored as 0).
        updates["leaf_ttl_hours"] = (
            data.get("leaf_ttl_hours", ["0"])[0] if "leaf_on" in data else "0")
        updates["default_monthly_mb"] = (
            data.get("default_monthly_mb", ["0"])[0] if "mb_on" in data else "0")
        # Speed is entered in kbps; stored internally as mbit.
        if "bw_on" in data:
            try:
                kbps = float(data.get("default_bw_kbps", ["0"])[0])
            except ValueError:
                kbps = 0.0
            updates["default_bw_mbit"] = kbps / 1000.0
        else:
            updates["default_bw_mbit"] = 0
        wtdb.save_settings(updates)
        want_ap = "ap_on" in data
        if want_ap != ap_is_running():
            subprocess.run(
                ["systemctl", "start" if want_ap else "stop", "hostapd"], timeout=15)
        return self._send(settings_page(ok="Saved. Leaf changes apply within ~30s."))

    def _do_customize(self, data):
        if data.get("action", [""])[0] == "reset":
            wtconfig.save(dict(wtconfig.DEFAULTS))
            return self._send(customize_page(wtconfig.load(), ok="Reset to defaults."))
        updates = {k: data.get(k, [""])[0] for k in wtconfig.DEFAULTS}
        cfg = wtconfig.save(updates)
        return self._send(customize_page(cfg, ok="Saved! It's live on the portal now."))

    def _do_change_password(self, data):
        current = data.get("current", [""])[0]
        new1 = data.get("new1", [""])[0]
        new2 = data.get("new2", [""])[0]
        if not verify_password(current):
            return self._send(change_password_page(err="Current password is wrong."))
        if not new1 or new1 != new2:
            return self._send(change_password_page(err="New passwords don't match (or are empty)."))
        if len(new1) < 8:
            return self._send(change_password_page(err="New password must be at least 8 characters."))
        save_password(new1)
        # Keep this session valid, log out every other device.
        my_token = self._token()
        for t in list(SESSIONS):
            if t != my_token:
                del SESSIONS[t]
        return self._send(change_password_page(ok="Password changed. Other devices have been logged out."))

    def _do_action(self, data):
        mac = (data.get("mac", [""])[0] or "").lower()
        action = data.get("action", [""])[0]
        if not mac or not wtdb.get_user(mac):
            return
        try:
            if action == "rename":
                wtdb.set_name(mac, (data.get("name", [""])[0] or "").strip()[:40])
            elif action == "set-bw":
                v = data.get("bw", [""])[0]
                if v != "":
                    wtdb.set_bw_limit(mac, float(v) / 1000.0)  # kbps -> mbit
            elif action == "clear-bw":
                wtdb.set_bw_limit(mac, None)
            elif action == "set-limit":
                v = data.get("limit", [""])[0]
                if v != "":
                    wtdb.set_monthly_limit(mac, float(v))
            elif action == "reset":
                wtdb.reset_usage(mac)
            elif action == "force":
                wtdb.set_force_full_speed(mac, True)
            elif action == "unforce":
                wtdb.set_force_full_speed(mac, False)
            elif action == "remove":
                wtdb.delete_user(mac)
        except (ValueError, TypeError):
            pass


# -------------------------------------------------------------------- main ---
def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--set-password":
        import getpass
        p1 = getpass.getpass("New admin password: ")
        p2 = getpass.getpass("Repeat: ")
        if p1 != p2 or not p1:
            print("Passwords don't match / empty — aborted.")
            sys.exit(1)
        save_password(p1)
        print(f"Password saved to {CONF}.")
        return

    if "password_hash" not in load_conf():
        sys.stderr.write(f"No password set. Run: {sys.argv[0]} --set-password\n")
        sys.exit(1)

    threading.Thread(target=sampler, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    sys.stderr.write(f"wifitree-webadmin listening on :{PORT} (LAN side only)\n")
    srv.serve_forever()


if __name__ == "__main__":
    main()
