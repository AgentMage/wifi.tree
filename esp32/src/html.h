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

// ── Shared chrome for dynamically-built pages (status card, admin) ────────────
// Use: send PORTAL_HEAD, then a snprintf'd body, then PORTAL_FOOT.
static const char PORTAL_HEAD[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>wifi.tree</title>"
    "<style>" PORTAL_CSS "</style>"
    "</head><body>"
    "<div class='wrap'>"
    "<div class='logo'>&#x1F333;</div>"
    "<h1>wifi.tree</h1>"
    "<p class='tag'>community wifi &middot; please be mindful, it&#39;s shared</p>";

static const char PORTAL_FOOT[] =
    "<p class='foot'>Shared, fair, bandwidth-limited.<br>Be kind, keep it light.</p>"
    "</div></body></html>";

// ── Portal welcome page (GET /) ───────────────────────────────────────────────
static const char PORTAL_WELCOME_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>wifi.tree</title>"
    "<style>" PORTAL_CSS "</style>"
    "</head><body>"
    "<div class='wrap'>"
    "<div class='logo'>&#x1F333;</div>"
    "<h1>wifi.tree</h1>"
    "<p class='tag'>community wifi &middot; please be mindful, it&#39;s shared</p>"
    "<div class='card'>"
    "<p class='headline' style='color:#2e7d32'>Welcome to the gathering &#x1F33F;</p>"
    "<p class='sub'>This is shared, free, community wifi. Enter a name and "
    "grow a leaf to get online for 3&nbsp;hours.</p>"
    "</div>"
    "<div class='card'>"
    "<form method='POST'>"
    "<input name='name' placeholder='enter your name' maxlength='40' required>"
    "<button type='submit'>Grow a Leaf &#x1F33F;</button>"
    "</form>"
    "</div>"
    "<p class='foot'>Shared, fair, bandwidth-limited.<br>Be kind, keep it light.</p>"
    "</div>"
    "</body></html>";

// ── Portal success page, split around the visitor's name (POST /) ─────────────
// Send: PORTAL_SUCCESS_A + html-escaped name + PORTAL_SUCCESS_B
static const char PORTAL_SUCCESS_A[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>wifi.tree</title>"
    "<style>" PORTAL_CSS "</style>"
    "</head><body>"
    "<div class='wrap'>"
    "<div class='logo'>&#x1F333;</div>"
    "<h1>wifi.tree</h1>"
    "<p class='tag'>community wifi &middot; please be mindful, it&#39;s shared</p>"
    "<div class='card ok leafy'>"
    "<div class='success'>"
    "<div class='leaf-burst'><span class='sway'>&#x1F33F;</span></div>"
    "<p class='leaf-sub'>a fresh leaf, just for you</p>"
    "<div class='big'>You grew a leaf, ";

static const char PORTAL_SUCCESS_B[] =
    "!</div>"
    "<p class='note'>You&#39;re online &mdash; close this page and start browsing.<br>"
    "Your leaf stays fresh for 3&nbsp;hours.</p>"
    "</div>"
    "<a class='btnlink' href='http://wifi.tree'>Go to wifi.tree &rarr;</a>"
    "</div>"
    "<p class='foot'>Shared, fair, bandwidth-limited.<br>Be kind, keep it light.</p>"
    "</div>"
    "</body></html>";
