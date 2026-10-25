#!/usr/bin/env python3
"""Local server for the browser build (this machine only).

Threads in WebAssembly need SharedArrayBuffer, which browsers only enable on cross-origin isolated
pages, so every response carries COOP/COEP headers.

  /            web/ (the page)
  /build/      build_web/www (recomp.js / .wasm from build_web.sh)

The page never downloads game files: the player picks their own game folder, which the browser copies
into its private storage.

Usage: python3 web/serve.py [--port 8007]
"""
import argparse
import http.server
import json
import os
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(os.path.dirname(HERE), "build_web", "www")
ROOT = os.path.dirname(HERE)
GAME = None   # set by --game: a local copy of the game, served at /game/ (this machine only)


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if GAME and urllib.parse.urlparse(self.path).path == "/game/manifest.json":
            files = [{"path": os.path.relpath(os.path.join(d, f), GAME), "size": os.path.getsize(os.path.join(d, f))}
                     for d, _, fs in os.walk(GAME) for f in fs]
            body = json.dumps(files).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlparse(path).path)
        if GAME and p.startswith("/game/"):
            return os.path.join(GAME, p[len("/game/"):])
        if p.startswith("/build/"):
            return os.path.join(BUILD, p[len("/build/"):])
        return os.path.join(HERE, p.lstrip("/"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8007)
    ap.add_argument("--game", default=os.path.join(ROOT, "work", "run_web"),
                    help="game folder to serve at /game/ so the page loads it without a picker (default: work/run_web if present)")
    a = ap.parse_args()
    global GAME
    GAME = a.game if os.path.isdir(a.game) else None
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"http://localhost:{a.port}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
