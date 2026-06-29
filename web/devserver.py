#!/usr/bin/env python3
"""
wifi.mom dev server — local preview that mirrors the production nginx config.

Replicates nginx's `try_files $uri $uri/ $uri.html`, so clean URLs like
/visitor and /docs/esp32 resolve to their .html files just like in the
Docker/nginx container. For preview only; production is served by nginx.

    python3 devserver.py [port]   (default 8000)
"""
import http.server
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8000


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def translate_path(self, path):
        local = super().translate_path(path)
        # try_files: exact → dir/index → .html fallback
        if os.path.isdir(local):
            index = os.path.join(local, "index.html")
            if os.path.exists(index):
                return index
        if not os.path.exists(local):
            html = local.rstrip("/") + ".html"
            if os.path.exists(html):
                return html
        return local

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    addr = ("127.0.0.1", PORT)
    print(f"🌳 wifi.mom dev server → http://127.0.0.1:{PORT}")
    print("   clean URLs enabled (mirrors nginx). Ctrl-C to stop.")
    http.server.ThreadingHTTPServer(addr, Handler).serve_forever()
