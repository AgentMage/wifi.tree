import sqlite3, time, os, datetime, json

DB_PATH = "/var/lib/wifitree/wifitree.db"

# Factory defaults. These are also the fallback when settings.json is absent.
DEFAULT_MONTHLY_MB = 100
DEFAULT_BW_MBIT = 0.1
DEFAULT_LEAF_TTL_HOURS = 3.0
CHECKIN_TTL_SECONDS = 3 * 3600  # kept for backward-compat (factory value)

# Live, operator-editable global settings. Read fresh on every access so the
# admin panel can change them without restarting the daemons.
SETTINGS_PATH = "/etc/wifitree/settings.json"
SETTINGS_DEFAULTS = {
    "leaf_ttl_hours": DEFAULT_LEAF_TTL_HOURS,  # 0 => leaves never expire
    "default_monthly_mb": float(DEFAULT_MONTHLY_MB),  # 0 => unlimited data
    "default_bw_mbit": DEFAULT_BW_MBIT,  # 0 => uncapped (shared pool only)
}


def settings():
    s = dict(SETTINGS_DEFAULTS)
    try:
        with open(SETTINGS_PATH, encoding="utf-8") as f:
            raw = json.load(f)
        for k in SETTINGS_DEFAULTS:
            if k in raw:
                try:
                    s[k] = float(raw[k])
                except (TypeError, ValueError):
                    pass
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        pass
    return s


def save_settings(updates):
    s = settings()
    for k in SETTINGS_DEFAULTS:
        if k in updates:
            try:
                s[k] = max(0.0, float(updates[k]))
            except (TypeError, ValueError):
                pass
    os.makedirs(os.path.dirname(SETTINGS_PATH), exist_ok=True)
    with open(SETTINGS_PATH, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)
    os.chmod(SETTINGS_PATH, 0o644)
    return s


def leaf_ttl_hours():
    return settings()["leaf_ttl_hours"]


def checkin_ttl_seconds():
    """Seconds a leaf stays fresh, or None if leaves never expire."""
    h = settings()["leaf_ttl_hours"]
    return None if h <= 0 else int(h * 3600)


def default_monthly_mb():
    """Default monthly cap for new registrations. 0.0 means unlimited."""
    return settings()["default_monthly_mb"]


def default_bw_mbit():
    """Default per-device speed for new registrations. 0.0 means uncapped."""
    return settings()["default_bw_mbit"]

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    mac TEXT PRIMARY KEY,
    name TEXT,
    first_seen REAL,
    last_checkin REAL,
    bytes_used_month INTEGER DEFAULT 0,
    month_start TEXT,
    bw_limit_mbit REAL,
    monthly_limit_mb REAL DEFAULT 100,
    force_full_speed INTEGER DEFAULT 0
);
"""


META_SCHEMA = "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);"


def get_conn():
    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute(SCHEMA)
    conn.execute(META_SCHEMA)
    conn.commit()
    return conn


def _meta_get(conn, key, default=None):
    row = conn.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
    return row[0] if row else default


def _meta_set(conn, key, value):
    conn.execute(
        "INSERT INTO meta(key,value) VALUES(?,?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        (key, str(value)),
    )


def _ensure_planted(conn):
    """Record when Wifi Tree was 'planted' and seed the lifetime byte counter."""
    changed = False
    if _meta_get(conn, "planted_at") is None:
        row = conn.execute("SELECT MIN(first_seen) FROM users").fetchone()
        planted = row[0] if row and row[0] else time.time()
        _meta_set(conn, "planted_at", planted)
        changed = True
    if _meta_get(conn, "total_bytes") is None:
        row = conn.execute("SELECT COALESCE(SUM(bytes_used_month),0) FROM users").fetchone()
        _meta_set(conn, "total_bytes", int(row[0] or 0))
        changed = True
    if changed:
        conn.commit()


def get_stats():
    """Lifetime stats: total bytes served and when the tree was planted."""
    conn = get_conn()
    _ensure_planted(conn)
    total = int(_meta_get(conn, "total_bytes", "0") or 0)
    planted = float(_meta_get(conn, "planted_at", str(time.time())))
    users = conn.execute("SELECT COUNT(*) FROM users").fetchone()[0]
    conn.close()
    return {"total_bytes": total, "planted_at": planted, "users": users}


def cur_month():
    return datetime.date.today().strftime("%Y-%m")


def _row_to_dict(row, cols):
    return dict(zip(cols, row))


def _maybe_roll_month(conn, mac):
    m = cur_month()
    row = conn.execute("SELECT month_start FROM users WHERE mac=?", (mac,)).fetchone()
    if row and row[0] != m:
        conn.execute(
            "UPDATE users SET bytes_used_month=0, month_start=? WHERE mac=?", (m, mac)
        )
        conn.commit()


def upsert_checkin(mac, name):
    mac = mac.lower()
    conn = get_conn()
    now = time.time()
    row = conn.execute("SELECT mac FROM users WHERE mac=?", (mac,)).fetchone()
    if row:
        conn.execute(
            "UPDATE users SET name=?, last_checkin=?, "
            "bw_limit_mbit = COALESCE(bw_limit_mbit, ?) WHERE mac=?",
            (name, now, default_bw_mbit(), mac),
        )
    else:
        conn.execute(
            "INSERT INTO users (mac, name, first_seen, last_checkin, bytes_used_month, "
            "month_start, monthly_limit_mb, bw_limit_mbit) VALUES (?,?,?,?,0,?,?,?)",
            (mac, name, now, now, cur_month(), default_monthly_mb(), default_bw_mbit()),
        )
    conn.commit()
    _maybe_roll_month(conn, mac)
    conn.close()


def add_bytes(mac, delta_bytes):
    if delta_bytes <= 0:
        return
    mac = mac.lower()
    conn = get_conn()
    row = conn.execute("SELECT mac FROM users WHERE mac=?", (mac,)).fetchone()
    if not row:
        conn.execute(
            "INSERT INTO users (mac, name, first_seen, last_checkin, bytes_used_month, "
            "month_start, monthly_limit_mb) VALUES (?,?,?,?,0,?,?)",
            (mac, "(unregistered)", time.time(), 0, cur_month(), default_monthly_mb()),
        )
        conn.commit()
    _maybe_roll_month(conn, mac)
    conn.execute(
        "UPDATE users SET bytes_used_month = bytes_used_month + ? WHERE mac=?",
        (delta_bytes, mac),
    )
    _ensure_planted(conn)
    cur = int(_meta_get(conn, "total_bytes", "0") or 0)
    _meta_set(conn, "total_bytes", cur + int(delta_bytes))
    conn.commit()
    conn.close()


def get_user(mac):
    mac = mac.lower()
    conn = get_conn()
    cols = ["mac", "name", "first_seen", "last_checkin", "bytes_used_month",
            "month_start", "bw_limit_mbit", "monthly_limit_mb", "force_full_speed"]
    row = conn.execute(f"SELECT {','.join(cols)} FROM users WHERE mac=?", (mac,)).fetchone()
    conn.close()
    return _row_to_dict(row, cols) if row else None


def list_users():
    conn = get_conn()
    cols = ["mac", "name", "first_seen", "last_checkin", "bytes_used_month",
            "month_start", "bw_limit_mbit", "monthly_limit_mb", "force_full_speed"]
    rows = conn.execute(f"SELECT {','.join(cols)} FROM users ORDER BY last_checkin DESC").fetchall()
    conn.close()
    return [_row_to_dict(r, cols) for r in rows]


def is_full_speed(user):
    if not user:
        return False
    if user.get("force_full_speed"):
        return True
    if user.get("month_start") != cur_month():
        # stale row not yet rolled by daemon; treat as fresh quota
        used = 0
    else:
        used = user.get("bytes_used_month") or 0
    limit_mb = user.get("monthly_limit_mb")
    if limit_mb is None:
        limit_mb = default_monthly_mb()
    # limit_mb <= 0 means unlimited (quota check skipped)
    if limit_mb and limit_mb > 0 and used >= limit_mb * 1024 * 1024:
        return False
    ttl = checkin_ttl_seconds()
    if ttl is not None:  # None => leaves never expire
        last = user.get("last_checkin") or 0
        if time.time() - last > ttl:
            return False
    return True


def set_bw_limit(mac, mbit):
    conn = get_conn()
    conn.execute("UPDATE users SET bw_limit_mbit=? WHERE mac=?", (mbit, mac.lower()))
    conn.commit()
    conn.close()


def set_monthly_limit(mac, mb):
    conn = get_conn()
    conn.execute("UPDATE users SET monthly_limit_mb=? WHERE mac=?", (mb, mac.lower()))
    conn.commit()
    conn.close()


def set_name(mac, name):
    conn = get_conn()
    conn.execute("UPDATE users SET name=? WHERE mac=?", (name, mac.lower()))
    conn.commit()
    conn.close()


def set_force_full_speed(mac, flag):
    conn = get_conn()
    conn.execute("UPDATE users SET force_full_speed=? WHERE mac=?", (1 if flag else 0, mac.lower()))
    conn.commit()
    conn.close()


def reset_usage(mac):
    conn = get_conn()
    conn.execute(
        "UPDATE users SET bytes_used_month=0, month_start=? WHERE mac=?",
        (cur_month(), mac.lower()),
    )
    conn.commit()
    conn.close()


def delete_user(mac):
    conn = get_conn()
    conn.execute("DELETE FROM users WHERE mac=?", (mac.lower(),))
    conn.commit()
    conn.close()


def is_registered(user):
    return bool(user and (user.get("last_checkin") or 0) > 0)


def find_by_name_or_mac(token):
    token = token.lower()
    conn = get_conn()
    cols = ["mac", "name", "first_seen", "last_checkin", "bytes_used_month",
            "month_start", "bw_limit_mbit", "monthly_limit_mb", "force_full_speed"]
    rows = conn.execute(f"SELECT {','.join(cols)} FROM users").fetchall()
    conn.close()
    matches = [_row_to_dict(r, cols) for r in rows]
    exact_mac = [u for u in matches if u["mac"] == token]
    if exact_mac:
        return exact_mac
    return [u for u in matches if u["name"] and u["name"].lower() == token]
