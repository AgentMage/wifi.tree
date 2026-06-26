#pragma once

// Split at the <select> content so we can inject the scanned network list
// via chunked sends without building a big intermediate string.

static const char SETUP_BEFORE_NETS[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>wifi.tree setup</title>"
    "<style>"
    "*{box-sizing:border-box}"
    "body{font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 20px;color:#222}"
    "h2{color:#2d7a2d}"
    "label{display:block;margin-top:14px;font-size:14px;font-weight:600}"
    "select,input{width:100%;padding:9px;margin-top:4px;border:1px solid #ccc;"
    "border-radius:4px;font-size:15px}"
    "button{margin-top:20px;width:100%;padding:12px;background:#2d7a2d;color:#fff;"
    "border:none;border-radius:4px;font-size:16px;cursor:pointer}"
    ".note{margin-top:12px;font-size:13px;color:#666}"
    "</style></head><body>"
    "<h2>wifi.tree setup</h2>"
    "<p>Choose the network to use as internet uplink.</p>"
    "<form action='/save' method='POST'>"
    "<label>Network</label>"
    "<select name='ssid'>";

static const char SETUP_AFTER_NETS[] =
    "</select>"
    "<label style='margin-top:10px;font-size:12px;opacity:.6'>"
    "Or type SSID manually (for hidden networks)</label>"
    "<input type='text' name='ssid_manual' placeholder='e.g. MyHiddenNetwork' autocomplete='off'>"
    "<label>Password</label>"
    "<input type='password' name='pass' placeholder='Leave blank for open networks'>"
    "<label>Admin password</label>"
    "<input type='password' name='admin_pass' "
    "placeholder='Protects wifi.tree/admin (can set later)'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form>"
    "<p class='note'>Device will reboot and broadcast <strong>wifi.tree</strong>.</p>"
    "</body></html>";

static const char SAVING_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>wifi.tree</title></head><body>"
    "<h2>Connecting&hellip;</h2>"
    "<p>wifi.tree is restarting. Reconnect to <strong>wifi.tree</strong> in a few seconds.</p>"
    "</body></html>";

// ── Real portal CSS (mirrors the Pi's wifitree-portal.py) ────────────────────
#define PORTAL_CSS \
  "*{box-sizing:border-box}" \
  "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;" \
    "background:#0b1a0f;color:#eaffea;margin:0;padding:0;min-height:100vh;" \
    "display:flex;flex-direction:column;align-items:center}" \
  ".wrap{width:100%;max-width:460px;padding:24px 18px 40px}" \
  ".logo{text-align:center;font-size:3.2em;line-height:1;margin:18px 0 4px}" \
  "h1{color:#9fe89f;text-align:center;font-size:1.7em;margin:0 0 2px}" \
  ".tag{text-align:center;opacity:.7;font-size:.95em;margin:0 0 22px}" \
  ".card{background:#12251a;border:1px solid #1f3d29;border-radius:16px;" \
    "padding:20px 18px;margin-bottom:16px}" \
  ".card.ok{border-color:#2e7d32}" \
  ".headline{font-size:1.25em;font-weight:700;margin:0 0 6px}" \
  ".card.ok .headline{color:#8ef08e}" \
  ".sub{opacity:.85;font-size:.95em;margin:0 0 6px;line-height:1.4}" \
  "form{margin:0}" \
  "input{font-size:1.15em;padding:14px;width:100%;border-radius:12px;" \
    "border:1px solid #2e7d32;background:#0b1a0f;color:#eaffea;margin-bottom:12px}" \
  "input::placeholder{color:#6a8a72}" \
  "button{font-size:1.2em;font-weight:700;padding:16px;width:100%;background:#2e7d32;" \
    "color:#fff;border:none;border-radius:12px;cursor:pointer}" \
  "button:active{background:#256528}" \
  ".success{text-align:center;padding:26px 18px}" \
  ".success .big{font-size:1.4em;font-weight:700;color:#8ef08e;margin-bottom:6px}" \
  ".note{opacity:.75;font-size:.85em;line-height:1.5}" \
  ".foot{text-align:center;opacity:.5;font-size:.8em;margin-top:14px;line-height:1.5}" \
  "a.btnlink{display:block;text-decoration:none;text-align:center;" \
    "font-size:1.15em;font-weight:700;padding:14px;margin-top:12px;" \
    "background:#2e7d32;color:#fff;border-radius:12px}" \
  ".card.ok.leafy{background:radial-gradient(circle at 50% 0,#163a20,#0f2417 70%)}" \
  ".leaf-burst{font-size:4.6em;line-height:1;margin:4px 0 2px;display:inline-block;" \
    "animation:leafgrow .75s cubic-bezier(.2,.8,.3,1.3) both}" \
  ".leaf-burst .sway{display:inline-block;transform-origin:50% 90%;" \
    "animation:leafsway 3.2s ease-in-out infinite}" \
  ".leaf-sub{color:#8ef08e;font-weight:700;font-size:.95em;letter-spacing:.04em;" \
    "text-transform:uppercase;opacity:.9;margin-bottom:2px}" \
  "@keyframes leafgrow{" \
    "0%{transform:scale(0) rotate(-45deg);opacity:0}" \
    "60%{transform:scale(1.18) rotate(10deg);opacity:1}" \
    "100%{transform:scale(1) rotate(0);opacity:1}}" \
  "@keyframes leafsway{0%,100%{transform:rotate(-7deg)}50%{transform:rotate(7deg)}}"

// Sendable form of PORTAL_CSS (so dynamic portal pages can chunk it out).
static const char PORTAL_CSS_STR[] = PORTAL_CSS;

// ── Admin chrome (mirrors the Pi's wifitree-webadmin.py theme) ────────────────
#define ADMIN_CSS \
  "*{box-sizing:border-box}" \
  "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;" \
    "background:#0b1a0f;color:#eaffea;margin:0}" \
  "header{background:#12251a;border-bottom:1px solid #1f3d29;padding:14px 18px;" \
    "display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px}" \
  "header h1{margin:0;font-size:1.2em;color:#9fe89f}" \
  "header nav a,header nav form{color:#9fe89f;text-decoration:none;font-size:.9em;opacity:.85}" \
  "header nav a.on{opacity:1;text-decoration:underline}" \
  ".wrap{max-width:900px;margin:0 auto;padding:18px}" \
  ".cards{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:18px}" \
  ".stat{background:#12251a;border:1px solid #1f3d29;border-radius:12px;" \
    "padding:12px 16px;flex:1;min-width:96px}" \
  ".stat .n{font-size:1.5em;font-weight:700;color:#9fe89f}" \
  ".stat .l{font-size:.75em;opacity:.7;margin-top:2px}" \
  "h2{color:#9fe89f;font-size:1em;border-bottom:1px solid #1f3d29;" \
    "padding-bottom:6px;margin:22px 0 10px}" \
  "table{width:100%;border-collapse:collapse;font-size:.88em}" \
  "th,td{text-align:left;padding:7px 8px;border-bottom:1px solid #1a2f20;vertical-align:middle}" \
  "th{font-size:.72em;text-transform:uppercase;opacity:.6;font-weight:600}" \
  ".pill{display:inline-block;padding:2px 9px;border-radius:99px;font-size:.74em;font-weight:600}" \
  ".pill.ok{background:#1b3a1f;color:#8ef08e}" \
  ".pill.warn{background:#3a2f15;color:#ffd54f}" \
  ".pill.bad{background:#3a1b1b;color:#ff8a80}" \
  ".bar{height:7px;background:#0b1a0f;border:1px solid #1f3d29;border-radius:99px;" \
    "overflow:hidden;width:80px;display:inline-block;vertical-align:middle}" \
  ".bar .f{display:block;height:100%;border-radius:99px;min-width:2px}" \
  "form.inline{display:inline;margin:0}" \
  "input[type=number],input[type=text],input[type=password]{background:#0b1a0f;" \
    "border:1px solid #2e7d32;color:#eaffea;border-radius:7px;padding:6px 8px;font-size:.9em}" \
  "button{background:#2e7d32;color:#fff;border:none;border-radius:7px;" \
    "padding:7px 11px;font-size:.85em;cursor:pointer;margin:1px}" \
  "button.sec{background:#33503a}button.danger{background:#a33}" \
  ".muted{opacity:.55;font-size:.85em}" \
  ".card2{background:#12251a;border:1px solid #1f3d29;border-radius:14px;padding:16px 18px;margin-bottom:16px}" \
  ".login{max-width:340px;margin:9vh auto;background:#12251a;border:1px solid #1f3d29;" \
    "border-radius:16px;padding:26px}" \
  ".login input{width:100%;padding:12px;font-size:1.05em;margin-bottom:12px}" \
  ".login button{width:100%;padding:13px;font-size:1.05em}" \
  ".login h1{color:#9fe89f;text-align:center;margin:.2em 0}" \
  ".cust label{display:block;margin:14px 0 4px;font-size:.78em;opacity:.75;text-transform:uppercase;letter-spacing:.03em}" \
  ".cust input[type=text],.cust textarea{width:100%;padding:10px;border-radius:9px;" \
    "border:1px solid #2e7d32;background:#0b1a0f;color:#eaffea;font-size:1em;font-family:inherit}" \
  ".cust textarea{min-height:54px;resize:vertical}" \
  ".cust input[type=color]{width:50px;height:36px;padding:2px;border:1px solid #2e7d32;" \
    "background:#0b1a0f;border-radius:8px;vertical-align:middle}" \
  ".emoji-pick{margin-top:6px;display:flex;flex-wrap:wrap;gap:4px}" \
  ".emoji-pick button{font-size:1.3em;width:auto;padding:4px 7px;background:#18301d;margin:0}" \
  ".preview{background:#0b1a0f;border:1px solid #1f3d29;border-radius:14px;padding:18px;" \
    "text-align:center;margin-bottom:16px}" \
  ".preview .pe{font-size:2.6em;line-height:1}.preview .pt{font-size:1.3em;font-weight:700;margin:2px 0}" \
  ".preview .pg{opacity:.7;font-size:.9em}" \
  ".preview .pb{background:#2a2410;border:1px solid #b8860b;color:#ffe9a8;border-radius:10px;" \
    "padding:8px 10px;margin-top:10px;white-space:pre-wrap}"

// Nav shown on authed admin pages.
#define ADMIN_NAV \
  "<nav><a href='/admin'>dashboard</a> &middot; " \
  "<a href='/admin/settings'>settings</a> &middot; " \
  "<a href='/admin/customize'>customize</a> &middot; " \
  "<a href='/admin/logout'>log out</a></nav>"

// Emoji quick-pick buttons for the customize page (UTF-8 literals).
#define EMOJI_PICK \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8C\xB3</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8C\xB2</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8C\xBF</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8D\x84</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x94\xA5</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8C\x88</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xE2\x9B\xBA</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8E\xB8</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xE2\x9C\xA8</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8C\xBB</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x8C\x99</button>" \
  "<button type='button' onclick='setE(this.textContent)'>\xF0\x9F\x92\x9A</button>"

// Live-preview script for the customize page (no '%').
#define CUSTOMIZE_JS \
  "<script>" \
  "function gv(id){return document.getElementById(id).value}" \
  "function setE(e){document.getElementById('f-emoji').value=e;upd()}" \
  "function upd(){" \
  "document.getElementById('p-emoji').textContent=gv('f-emoji');" \
  "document.getElementById('p-title').textContent=gv('f-title');" \
  "document.getElementById('p-title').style.color=gv('f-accent');" \
  "document.getElementById('p-tag').textContent=gv('f-tag');" \
  "var b=gv('f-banner'),pb=document.getElementById('p-banner');" \
  "pb.textContent=b;pb.style.display=b.trim()?'block':'none'}" \
  "</script>"

// Authed page chrome: header + nav, then body, then ADMIN_FOOT.
static const char ADMIN_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>wifi.tree admin</title><style>" ADMIN_CSS "</style></head><body>"
    "<header><h1>&#x1F333; wifi.tree &mdash; admin</h1>" ADMIN_NAV "</header>"
    "<div class='wrap'>";

// Bare chrome for unauthenticated pages (login / set password) — no nav.
static const char ADMIN_HEAD_BARE[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>wifi.tree admin</title><style>" ADMIN_CSS "</style></head><body>";

static const char ADMIN_FOOT[] = "</div></body></html>";

// Dashboard live-poll script. Polls /admin/stats.json every 2s, computes
// per-device rates from byte deltas, and updates the stat cards + table.
// (Kept free of '%' so it can be emitted via snprintf with a "%s" format.)
#define ADMIN_DASH_JS \
"<script>" \
"var prev={},pt=0;" \
"function hbps(b){var u=['bps','Kbps','Mbps'],i=0;while(b>=1000&&i<2){b/=1000;i++}return b.toFixed(i?1:0)+' '+u[i]}" \
"function hb(k){var u=['KB','MB','GB'],i=0;while(k>=1024&&i<2){k/=1024;i++}return k.toFixed(i?1:0)+' '+u[i]}" \
"function esc(s){return s.replace(/[&<>]/g,function(c){return c=='&'?'&amp;':c=='<'?'&lt;':'&gt;'})}" \
"function tick(){fetch('/admin/stats.json').then(function(r){return r.json()}).then(function(d){" \
"document.getElementById('c-conn').textContent=d.conn;" \
"document.getElementById('c-ch').textContent=d.ch;" \
"var now=Date.now()/1000,dt=Math.max(now-pt,0.5),aD=0,aU=0;" \
"var rows=d.clients.map(function(c){" \
"var p=prev[c.ip]||{dn:c.dn,up:c.up};" \
"var dn=Math.max((c.dn-p.dn)*1024*8/dt,0),up=Math.max((c.up-p.up)*1024*8/dt,0);" \
"aD+=dn;aU+=up;prev[c.ip]={dn:c.dn,up:c.up};" \
"var nm=c.name?esc(c.name):'<span class=muted>&mdash;</span>';" \
"var h=c.host?esc(c.host):'';" \
"return '<tr><td>'+c.ip+'<div class=muted>'+h+'</div></td><td>'+nm+'</td><td>'+c.rssi+' dBm</td><td>'+hbps(dn)+'</td><td>'+hbps(up)+'</td><td>'+hb(c.dn+c.up)+'</td></tr>'" \
"}).join('');" \
"document.getElementById('live').innerHTML=d.clients.length?rows:'<tr><td colspan=6 class=muted>(nobody connected)</td></tr>';" \
"document.getElementById('c-down').textContent=hbps(aD);" \
"document.getElementById('c-up').textContent=hbps(aU);" \
"pt=now}).catch(function(e){})}" \
"tick();setInterval(tick,2000);" \
"</script>"

// (Portal welcome/success/status pages are now built dynamically in
// http_server.c from the operator's portal_cfg appearance settings.)
