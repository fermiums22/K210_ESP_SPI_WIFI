#!/usr/bin/env python3
from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path


def recv_line(sock: socket.socket, timeout_s: float) -> str:
    sock.settimeout(timeout_s)
    data = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            break
        data += b
        if b == b"\n":
            break
    return data.decode("utf-8", errors="replace").strip()


def put_file(host: str, port: int, path: Path, remote_name: str, timeout_s: float) -> None:
    size = path.stat().st_size
    print(f"TCP PUT {path} -> {host}:{port}/{remote_name} ({size} bytes)")
    start = time.monotonic()
    with socket.create_connection((host, port), timeout=timeout_s) as sock:
        sock.settimeout(timeout_s)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(f"PUT {remote_name} {size}\n".encode("ascii"))
        sent = 0
        with path.open("rb") as f:
            while True:
                chunk = f.read(4096)
                if not chunk:
                    break
                sock.sendall(chunk)
                sent += len(chunk)
                if sent == size or sent % 32768 == 0:
                    print(f"progress {sent}/{size}")
        response = recv_line(sock, timeout_s)
    elapsed = time.monotonic() - start
    print(f"response: {response or '<empty>'} ({elapsed:.1f}s)")
    if not response.startswith("OK"):
        raise SystemExit(1)


def main() -> int:
    ap = argparse.ArgumentParser(description="Send one file to KESP TCP bridge")
    ap.add_argument("host", help="ESP IP, for fallback AP use 192.168.4.1")
    ap.add_argument("file", help="Local file to send")
    ap.add_argument("remote_name", nargs="?", help="Name on K210 SD, max 20 chars recommended")
    ap.add_argument("--port", type=int, default=7777)
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    path = Path(args.file)
    if not path.exists() or not path.is_file():
        raise SystemExit(f"File not found: {path}")
    remote_name = args.remote_name or path.name
    if len(remote_name.encode("utf-8")) > 20:
        print("WARNING: current SPI frame carries only first 20 bytes of the file name", file=sys.stderr)
    put_file(args.host, args.port, path, remote_name, args.timeout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
