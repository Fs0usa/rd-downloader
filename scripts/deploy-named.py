#!/usr/bin/env python3
"""Send the ELF by URI so elfldr uses its real filename as the process name."""

from __future__ import annotations

import argparse
import http.server
import os
import pathlib
import signal
import socket
import subprocess
import sys
import threading
import urllib.parse


def route_address(host: str, port: int) -> tuple[str, str]:
    remote = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_DGRAM)[0][4][0]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect((remote, port))
        return remote, probe.getsockname()[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="PS5 address running elfldr")
    parser.add_argument("--port", type=int, default=9021)
    parser.add_argument("--deploy", required=True, help="prospero-deploy executable")
    parser.add_argument("payload", type=pathlib.Path)
    args = parser.parse_args()

    payload = args.payload.resolve(strict=True)
    data = payload.read_bytes()
    if not data.startswith(b"\x7fELF"):
        parser.error(f"not an ELF file: {payload}")
    remote, local = route_address(args.host, args.port)
    requested = threading.Event()
    filename = payload.name

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802
            if urllib.parse.urlsplit(self.path).path != "/" + filename:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(data)
                self.wfile.flush()
                requested.set()
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, fmt: str, *values: object) -> None:
            print("ELF loader request: " + fmt % values, file=sys.stderr)

    try:
        server = http.server.ThreadingHTTPServer((local, 0), Handler)
    except OSError as exc:
        print(f"Could not open temporary LAN HTTP server: {exc}", file=sys.stderr)
        return 1
    server.daemon_threads = True
    server.timeout = 1
    serving = threading.Thread(target=server.serve_forever, daemon=True)
    serving.start()
    # Keep elfldr's stdio pipe detached from the long-running payload. Without
    # pipe=0, prospero-deploy/socat can wait for the payload process to exit.
    uri = f"http://{local}:{server.server_address[1]}/{urllib.parse.quote(filename)}?pipe=0"
    print(f"Asking elfldr at {remote}:{args.port} to load {filename} by URI")
    print(f"Temporary ELF URL: {uri}")
    process: subprocess.Popen[bytes] | None = None
    try:
        process = subprocess.Popen(
            [args.deploy, "-h", args.host, "-p", str(args.port), uri],
            start_new_session=True,
        )
        deadline = 90
        while deadline > 0 and not requested.wait(timeout=0.1):
            deadline -= 0.1
            if process.poll() is not None:
                print(f"prospero-deploy exited with status {process.returncode} before elfldr fetched the ELF",
                      file=sys.stderr)
                return process.returncode or 1
        if not requested.is_set():
            print("elfldr did not fetch the ELF within 90 seconds", file=sys.stderr)
            return 1
        print(f"elfldr fetched {filename}; process name should now be {filename}")
        return 0
    finally:
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
        server.shutdown()
        server.server_close()
        serving.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
