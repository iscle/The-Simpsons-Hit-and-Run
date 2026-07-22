#!/usr/bin/env python3
"""Dev server for the Emscripten build.

- Adds the COOP/COEP headers required for SharedArrayBuffer (pthreads);
  plain `python -m http.server` will not work.
- Optionally mounts a game-asset directory under /assets/ with HTTP Range
  support, so the browser build can stream file regions on demand instead
  of preloading everything into memory.

Usage: python3 serve-wasm.py [port] [build-dir] [assets-dir]
Then open http://localhost:8080/SRR2.html
"""

import os
import re
import sys
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

ASSETS_PREFIX = "/assets/"
ASSETS_DIR = None


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {
        **SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".data": "application/octet-stream",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        super().end_headers()

    def _asset_path(self):
        if ASSETS_DIR is None or not self.path.startswith(ASSETS_PREFIX):
            return None
        rel = self.path[len(ASSETS_PREFIX):].split("?")[0]
        rel = os.path.normpath(rel)
        if rel.startswith(("..", "/")):
            return None
        p = os.path.join(ASSETS_DIR, rel)
        return p if os.path.isfile(p) else None

    def do_HEAD(self):
        p = self._asset_path()
        if p is None:
            if self.path.startswith(ASSETS_PREFIX):
                self.send_error(404)
                return
            return super().do_HEAD()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(os.path.getsize(p)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        p = self._asset_path()
        if p is None:
            if self.path.startswith(ASSETS_PREFIX):
                self.send_error(404)
                return
            return super().do_GET()

        size = os.path.getsize(p)
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        is_partial = False
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng)
            if m:
                if m.group(1):
                    start = int(m.group(1))
                    end = int(m.group(2)) if m.group(2) else size - 1
                elif m.group(2):
                    start = max(0, size - int(m.group(2)))
                end = min(end, size - 1)
                if start > end or start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.end_headers()
                    return
                is_partial = True

        length = end - start + 1
        self.send_response(206 if is_partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if is_partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

        with open(p, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(1024 * 1024, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    return
                remaining -= len(chunk)

    def log_message(self, fmt, *args):
        pass  # keep the console quiet; range requests are chatty


def main():
    global ASSETS_DIR
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    directory = sys.argv[2] if len(sys.argv) > 2 else "."
    if len(sys.argv) > 3:
        ASSETS_DIR = os.path.abspath(sys.argv[3])
    handler = lambda *args, **kwargs: Handler(*args, directory=directory, **kwargs)
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    print(f"Serving {directory} at http://localhost:{port} (with COOP/COEP)")
    if ASSETS_DIR:
        print(f"Streaming assets from {ASSETS_DIR} under {ASSETS_PREFIX} (Range enabled)")
    server.serve_forever()


if __name__ == "__main__":
    main()
