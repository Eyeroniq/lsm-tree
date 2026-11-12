#!/usr/bin/env python3
"""A browser terminal for recording the backend demo.

Serves terminal.html and runs a fixed list of demo commands for real in the
repository, streaming their output to the page (server-sent events). Only the
commands in ALLOWED can run.

    python3 demo/backend/terminal_server.py [port]    # from the repo root
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[2]
PAGE = Path(__file__).resolve().parent / "terminal.html"
ALLOWED = {
    "make test",
    "make asan-test CXX=clang++",
    "make tsan-test CXX=clang++",
    "ls -goh build/benchmark.db",
    "./demo/backend/demo.sh service",
    "make crash-test CRASH_ROUNDS=100",
    "make powercut-test POWERCUT_ROUNDS=60",
    "./demo/backend/demo.sh mutation",
    "make benchmark",
    "./demo/backend/demo.sh recovery",
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args) -> None:  # quiet
        pass

    def do_GET(self) -> None:
        url = urlparse(self.path)
        if url.path in ("/", "/index.html"):
            body = PAGE.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif url.path == "/run":
            command = parse_qs(url.query).get("cmd", [""])[0]
            if command not in ALLOWED:
                self.send_error(403, "command not allowed")
                return
            self.stream(command)
        else:
            self.send_error(404)

    def stream(self, command: str) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        # No "Entering directory" lines: they would print the home path.
        env = dict(os.environ, FORCE_COLOR="1", TERM="xterm-256color",
                   MAKEFLAGS="--no-print-directory")
        process = subprocess.Popen(
            ["bash", "-c", command], cwd=ROOT, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0,
        )
        assert process.stdout is not None
        buffer = b""
        while True:
            chunk = process.stdout.read(256)
            if not chunk:
                break
            buffer += chunk
            *lines, buffer = buffer.split(b"\n")
            for line in lines:
                self.send_event("line", line.decode("utf-8", "replace"))
        if buffer:
            self.send_event("line", buffer.decode("utf-8", "replace"))
        self.send_event("exit", str(process.wait()))

    def send_event(self, kind: str, data: str) -> None:
        self.wfile.write(f"event: {kind}\ndata: {json.dumps(data)}\n\n".encode())
        self.wfile.flush()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18180
    # All interfaces, because WSL forwards Windows localhost only to such
    # listeners. Only the commands in ALLOWED can ever run.
    host = sys.argv[2] if len(sys.argv) > 2 else "0.0.0.0"
    print(f"terminal on http://localhost:{port}", flush=True)
    ThreadingHTTPServer((host, port), Handler).serve_forever()
