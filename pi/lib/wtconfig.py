"""Shared, live-reloaded customization for the Wifi Tree captive portal.

Both the portal and the web admin import this. load() re-reads the file on
every call, so edits from the admin panel take effect on the next page view
with no service restart.
"""
import json, os, re

CONFIG_PATH = "/etc/wifitree/portal.json"

DEFAULTS = {
    "emoji": "🌳",
    "title": "wifi.tree",
    "tagline": "community wifi · please be mindful, it's shared",
    "banner": "",  # optional announcement; shown on the portal when non-empty
    "welcome_heading": "Welcome to the gathering 🌿",
    "welcome_text": ("This is shared, free, community wifi. Enter a name and "
                     "grow a leaf to get online for 3 hours."),
    "footer": "Shared, fair, bandwidth-limited.\nBe kind, keep it light.",
    "accent": "#2e7d32",
}

# Per-field length caps so the admin can't accidentally (or jokingly) blow up
# the page.
MAXLEN = {
    "emoji": 16, "title": 40, "tagline": 120, "banner": 200,
    "welcome_heading": 80, "welcome_text": 400, "footer": 200, "accent": 7,
}


def safe_accent(val):
    return val if re.fullmatch(r"#[0-9a-fA-F]{6}", val or "") else DEFAULTS["accent"]


def load():
    d = dict(DEFAULTS)
    try:
        with open(CONFIG_PATH, encoding="utf-8") as f:
            raw = json.load(f)
        for k in DEFAULTS:
            if k in raw and isinstance(raw[k], str):
                d[k] = raw[k][:MAXLEN[k]]
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        pass
    d["accent"] = safe_accent(d["accent"])
    return d


def save(updates):
    d = load()
    for k in DEFAULTS:
        if k in updates and isinstance(updates[k], str):
            v = updates[k][:MAXLEN[k]]
            d[k] = safe_accent(v) if k == "accent" else v
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
    os.chmod(CONFIG_PATH, 0o644)
    return d
