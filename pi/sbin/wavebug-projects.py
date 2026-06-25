#!/usr/bin/env python3
"""wavebug project manager — LAN-side admin for toggling experimental projects on/off.

Run with --set-password to initialise the password, then open
http://wavebug.local:8091 from any device on the home LAN.
Blocked on the 10.42.0.x AP side.
"""
import sys, os, time, html, json, secrets, hashlib, subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
from http.cookies import SimpleCookie

os.environ["PATH"] = os.environ.get("PATH", "") + ":/usr/sbin:/sbin:/usr/bin:/bin"

PORT        = 8091
CONF        = "/etc/wavebug/admin.conf"
PROJ_FILE   = "/etc/wavebug/projects.json"
AP_PREFIX   = "10.42.0."
SESSION_TTL = 12 * 3600
SESSIONS    = {}

DEFAULT_PROJECTS = {"projects": [
    {
        "id": "wifitree",
        "name": "wifi.tree",
        "description": "Community wifi AP with captive portal and usage metering",
        "services": ["hostapd", "wifitree-portal", "wifitree-accountd", "wifitree-webadmin"],
        "admin_url": "http://wavebug.local:8090"
    }
]}


# ---------------------------------------------------------------- password ---
def _hash_pw(pw):
    salt = secrets.token_hex(16)
    h = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), 200000)
    return f"{salt}${h.hex()}"

def verify_pw(pw):
    stored = _load_conf().get("password_hash", "")
    if "$" not in stored:
        return False
    salt, want = stored.split("$", 1)
    try:
        h = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), 200000)
    except ValueError:
        return False
    return secrets.compare_digest(h.hex(), want)

def _load_conf():
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

def save_pw(pw):
    os.makedirs(os.path.dirname(CONF), exist_ok=True)
    with open(CONF, "w") as f:
        f.write("# wavebug project manager config\n")
        f.write(f"password_hash = {_hash_pw(pw)}\n")
    os.chmod(CONF, 0o600)


# ---------------------------------------------------------------- projects ---
def load_projects():
    try:
        with open(PROJ_FILE) as f:
            return json.load(f)
    except FileNotFoundError:
        _seed_projects()
        return DEFAULT_PROJECTS
    except json.JSONDecodeError as ex:
        return {"projects": [], "_error": f"projects.json parse error: {ex}"}

def save_projects(data):
    os.makedirs(os.path.dirname(PROJ_FILE), exist_ok=True)
    with open(PROJ_FILE, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")

def _seed_projects():
    os.makedirs(os.path.dirname(PROJ_FILE), exist_ok=True)
    if not os.path.exists(PROJ_FILE):
        save_projects(DEFAULT_PROJECTS)

def _svc_active(name):
    try:
        r = subprocess.run(["systemctl", "is-active", name],
                           capture_output=True, text=True, timeout=3)
        return r.stdout.strip() == "active"
    except Exception:
        return False

def _proj_states(proj):
    return [_svc_active(s) for s in proj.get("services", [])]

def toggle_project(proj, start):
    svcs = proj.get("services", [])
    if not svcs:
        return
    order = svcs if start else list(reversed(svcs))
    subprocess.run(["systemctl", "start" if start else "stop"] + order, timeout=30)


# ---------------------------------------------------------------------- UI ---
CSS = """
*{box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
 background:#0c0c1a;color:#dde0ff;margin:0}
header{background:#11112a;border-bottom:1px solid #23235a;padding:14px 20px;
 display:flex;align-items:center;justify-content:space-between}
header h1{margin:0;font-size:1.3em;color:#9898ff}
header a{color:#9898ff;text-decoration:none;font-size:.9em;opacity:.8}
header a:hover{opacity:1}
.wrap{max-width:860px;margin:0 auto;padding:20px}
h2{color:#9898ff;font-size:1em;border-bottom:1px solid #23235a;
 padding-bottom:5px;margin:24px 0 12px}
.proj{background:#11112a;border:1px solid #23235a;border-radius:14px;
 padding:18px 22px;margin-bottom:12px;display:flex;align-items:center;gap:16px;flex-wrap:wrap}
.proj.on{border-color:#2a3a8a}
.proj.partial{border-color:#5a4a10}
.proj-info{flex:1;min-width:200px}
.proj-name{font-size:1.1em;font-weight:700;color:#c0c0ff}
.proj-desc{font-size:.86em;opacity:.62;margin-top:3px}
.proj-svcs{font-size:.76em;opacity:.4;margin-top:5px;font-family:monospace}
.pill{display:inline-block;padding:3px 11px;border-radius:99px;
 font-size:.78em;font-weight:700}
.pill.on{background:#1a2060;color:#9898ff}
.pill.partial{background:#3a2e08;color:#ffd060}
.pill.off{background:#2a1818;color:#ff7070}
.proj-actions{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
button{background:#25256a;color:#fff;border:none;border-radius:8px;
 padding:7px 14px;font-size:.88em;cursor:pointer}
button:hover{background:#32328a}
button.danger{background:#6a2222}
button.danger:hover{background:#8a2e2e}
a.admin-link{font-size:.82em;color:#9898ff;opacity:.65;text-decoration:none}
a.admin-link:hover{opacity:1}
.login{max-width:340px;margin:9vh auto;background:#11112a;border:1px solid #23235a;
 border-radius:16px;padding:28px}
.login input[type=password]{width:100%;padding:13px;font-size:1.1em;margin-bottom:12px;
 background:#0c0c1a;border:1px solid #3535aa;color:#dde0ff;border-radius:8px}
.login button{width:100%;padding:14px;font-size:1.1em}
.login h1{color:#9898ff;text-align:center;margin-bottom:4px}
.err{color:#ff8080;text-align:center;margin-bottom:10px;font-size:.9em}
.ok-msg{color:#80d880;text-align:center;margin-bottom:10px;font-size:.9em}
.muted{opacity:.5;font-size:.85em}
code{font-family:monospace;opacity:.7}
textarea{width:100%;background:#0c0c1a;border:1px solid #3535aa;color:#dde0ff;
 border-radius:9px;padding:12px;font-family:monospace;font-size:.88em;
 min-height:320px;resize:vertical}
"""

def _page(title, body):
    return f"""<!DOCTYPE html><html><head><title>{title}</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>{CSS}</style></head><body>{body}</body></html>"""

def login_page(err=""):
    e = f'<p class="err">{html.escape(err)}</p>' if err else ""
    return _page("wavebug projects", f"""
<div class="login">
  <h1>⚗️ wavebug</h1>
  <p class="muted" style="text-align:center;margin-bottom:18px">project manager</p>
  {e}
  <form method="POST" action="/login">
    <input type="password" name="password" placeholder="admin password" autofocus required>
    <button type="submit">Log in</button>
  </form>
</div>""")

def dashboard_page(ok="", err=""):
    data    = load_projects()
    projs   = data.get("projects", [])
    load_err = data.get("_error", "")
    o    = f'<p class="ok-msg">{html.escape(ok)}</p>'  if ok       else ""
    emsg = f'<p class="err">{html.escape(err or load_err)}</p>' if (err or load_err) else ""
    cards = ""
    for p in projs:
        svcs   = p.get("services", [])
        states = _proj_states(p)
        all_on  = bool(svcs) and all(states)
        any_on  = any(states)
        partial = any_on and not all_on
        pill_cls = "partial" if partial else ("on" if all_on else "off")
        pill_txt = "partial" if partial else ("ON" if all_on else "OFF")
        proj_cls = "proj " + pill_cls
        btn_txt  = "turn off" if all_on else "turn on"
        btn_cls  = "danger"   if all_on else ""
        action   = "stop"     if all_on else "start"
        confirm  = (f"Turn off {p.get('name','')}? Stops all its services."
                    if all_on else f"Turn on {p.get('name','')}?")
        admin = ""
        if p.get("admin_url"):
            admin = f' <a class="admin-link" href="{html.escape(p["admin_url"])}" target="_blank">→ panel</a>'
        svc_txt = " &middot; ".join(html.escape(s) for s in svcs) if svcs else "<em>no services</em>"
        cards += f"""
<div class="{proj_cls}">
  <div class="proj-info">
    <div class="proj-name">{html.escape(p.get("name","(unnamed)"))}</div>
    <div class="proj-desc">{html.escape(p.get("description",""))}</div>
    <div class="proj-svcs">{svc_txt}</div>
  </div>
  <div class="proj-actions">
    <span class="pill {pill_cls}">{pill_txt}</span>
    {admin}
    <form method="POST" action="/toggle" style="margin:0">
      <input type="hidden" name="id"     value="{html.escape(p.get('id',''))}">
      <input type="hidden" name="action" value="{action}">
      <button class="{btn_cls}"
        onclick="return confirm('{html.escape(confirm)}')">{btn_txt}</button>
    </form>
  </div>
</div>"""
    if not projs and not load_err:
        cards = '<p class="muted">No projects yet — <a href="/edit" style="color:#9898ff">add one →</a></p>'
    return _page("wavebug — projects", f"""
<header>
  <h1>⚗️ wavebug — projects</h1>
  <span><a href="/edit">edit projects</a> &nbsp;·&nbsp; <a href="/logout">log out</a></span>
</header>
<div class="wrap">
  {o}{emsg}
  <h2>Projects</h2>
  {cards}
  <p class="muted" style="margin-top:18px">
    <a href="/edit" style="color:#9898ff">+ edit project list</a>
  </p>
</div>""")

def edit_page(ok="", err=""):
    try:
        with open(PROJ_FILE) as f:
            raw = f.read()
    except FileNotFoundError:
        raw = json.dumps(DEFAULT_PROJECTS, indent=2) + "\n"
    o    = f'<p class="ok-msg">{html.escape(ok)}</p>'  if ok  else ""
    emsg = f'<p class="err">{html.escape(err)}</p>'    if err else ""
    return _page("wavebug — edit projects", f"""
<header>
  <h1>⚗️ wavebug — projects</h1>
  <a href="/">&larr; back to dashboard</a>
</header>
<div class="wrap" style="max-width:660px">
  <h2>Edit project list</h2>
  {o}{emsg}
  <p class="muted">Each project needs <code>id</code> (unique slug), <code>name</code>,
  and <code>services</code> (list of systemd unit names to start/stop).
  Optional: <code>description</code>, <code>admin_url</code>.</p>
  <form method="POST" action="/edit">
    <textarea name="json" spellcheck="false">{html.escape(raw)}</textarea>
    <div style="margin-top:10px"><button type="submit">Save</button></div>
  </form>
</div>""")


# ----------------------------------------------------------------- handler ---
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def _ap_side(self):
        try: return self.connection.getsockname()[0].startswith(AP_PREFIX)
        except: return False

    def _token(self):
        c = SimpleCookie(self.headers.get("Cookie", ""))
        return c["wb_sess"].value if "wb_sess" in c else None

    def _authed(self):
        t = self._token()
        exp = SESSIONS.get(t)
        if exp and exp > time.time(): return True
        SESSIONS.pop(t, None)
        return False

    def _send(self, body, code=200, ct="text/html; charset=utf-8", hdrs=None):
        body = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (hdrs or []): self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _redir(self, loc, hdrs=None):
        self.send_response(303)
        self.send_header("Location", loc)
        for k, v in (hdrs or []): self.send_header(k, v)
        self.end_headers()

    def do_GET(self):
        if self._ap_side():
            return self._send("Not available here.", 403, "text/plain")
        path = urlparse(self.path).path
        if path == "/logout":
            SESSIONS.pop(self._token(), None)
            return self._redir("/", [("Set-Cookie", "wb_sess=; Max-Age=0; Path=/")])
        if not self._authed():
            return self._send(login_page())
        if path == "/edit":
            return self._send(edit_page())
        return self._send(dashboard_page())

    def do_POST(self):
        if self._ap_side():
            return self._send("Not available here.", 403, "text/plain")
        path   = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        data   = parse_qs(self.rfile.read(length).decode())

        if path == "/login":
            pw = data.get("password", [""])[0]
            if verify_pw(pw):
                tok = secrets.token_urlsafe(32)
                SESSIONS[tok] = time.time() + SESSION_TTL
                return self._redir("/", [("Set-Cookie",
                    f"wb_sess={tok}; HttpOnly; SameSite=Strict; Path=/; Max-Age={SESSION_TTL}")])
            return self._send(login_page("Wrong password."))

        if not self._authed():
            return self._redir("/")

        if path == "/toggle":
            proj_id = data.get("id",     [""])[0]
            action  = data.get("action", [""])[0]
            if action in ("start", "stop"):
                for p in load_projects().get("projects", []):
                    if p.get("id") == proj_id:
                        toggle_project(p, action == "start")
                        break
            return self._redir("/")

        if path == "/edit":
            raw = data.get("json", [""])[0]
            try:
                parsed = json.loads(raw)
                if not isinstance(parsed.get("projects"), list):
                    raise ValueError("top-level 'projects' key must be a list")
            except (json.JSONDecodeError, ValueError) as ex:
                return self._send(edit_page(err=f"Invalid JSON: {ex}"))
            save_projects(parsed)
            return self._send(edit_page(ok="Saved."))

        return self._send("not found", 404, "text/plain")


# -------------------------------------------------------------------- main ---
def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--set-password":
        import getpass
        p1 = getpass.getpass("New admin password: ")
        p2 = getpass.getpass("Repeat: ")
        if not p1 or p1 != p2:
            print("Passwords don't match or empty — aborted.")
            sys.exit(1)
        save_pw(p1)
        print(f"Password saved to {CONF}.")
        return

    if "password_hash" not in _load_conf():
        sys.stderr.write(f"No password set. Run:  sudo {sys.argv[0]} --set-password\n")
        sys.exit(1)

    _seed_projects()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    sys.stderr.write(f"wavebug-projects listening on :{PORT}\n")
    srv.serve_forever()

if __name__ == "__main__":
    main()
