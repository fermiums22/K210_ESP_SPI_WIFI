#!/usr/bin/env python3
from pathlib import Path
import socket
import sys
import time


def main(argv):
    if len(argv) < 4:
        print("usage: wifi_put.py <host> <local> <remote> [port]")
        return 1
    host = argv[1]
    local = Path(argv[2])
    remote = argv[3].replace("\\", "/")
    port = int(argv[4]) if len(argv) > 4 else 7777
    data_len = local.stat().st_size

    start = time.time()
    sent = 0
    with socket.create_connection((host, port), timeout=10) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.sendall(f"PUT {remote} {data_len}\n".encode("ascii"))
        with local.open("rb") as f:
            while True:
                chunk = f.read(32 * 1024)
                if not chunk:
                    break
                s.sendall(chunk)
                sent += len(chunk)
                now = time.time()
                if now > start:
                    kb = sent / 1024
                    rate = kb / (now - start)
                    print(f"{sent}/{data_len}  {rate:.1f} kB/s", flush=True)
        s.shutdown(socket.SHUT_WR)
        resp = s.recv(128)
    dt = max(time.time() - start, 0.001)
    print(f"done {sent} bytes in {dt:.2f}s, {sent / 1024 / dt:.1f} kB/s, resp={resp!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
