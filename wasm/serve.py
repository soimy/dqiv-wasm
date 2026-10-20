#!/usr/bin/env python3
"""Serve the build directory, refusing to let the browser cache anything.

python3 -m http.server sends no cache headers, so a plain reload can reuse the
old dqiv_wasm_game.wasm: the loader fetches it by name, and Chrome is free to
treat a response without validators as fresh. That has cost two rounds of "the
change is not there" already, so this server sets no-store on everything.

    cd wasm/build && python3 ../serve.py 8111
"""
import functools
import http.server
import sys


class NoStore(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def log_message(self, fmt, *args):
        # The page polls nothing, so a quiet log is the useful one.
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8111
    handler = functools.partial(NoStore, directory=".")
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), handler) as server:
        print(f"serving the build on http://127.0.0.1:{port}/index.html (no-store)")
        server.serve_forever()
