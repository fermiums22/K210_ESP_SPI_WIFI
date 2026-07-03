#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import socket
import time
from pathlib import Path

KSD_MAGIC = b"KSD1\n"
DEFAULT_KSD_BAUD = 921600
DEFAULT_TCP_PORT = 18080
DEFAULT_SIZE = 1024 * 1024
TCP_CHUNK = 16 * 1024


def make_test_file(path: Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.stat().st_size == size:
        return
    with path.open("wb") as f:
        pos = 0
        while pos < size:
            n = min(65536, size - pos)
            data = bytes(((pos + i) * 37 + ((pos + i) >> 8)) & 0xFF for i in range(n))
            f.write(data)
            pos += n


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def tcp_put(host: str, port: int, local: Path, remote: str, timeout: float) -> float:
    size = local.stat().st_size
    print(f"TCP PUT: {local} -> {host}:{port}/{remote} ({size} bytes)")
    start = time.monotonic()
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(f"PUT {remote} {size}\n".encode("ascii"))
        sent = 0
        with local.open("rb") as f:
            while True:
                chunk = f.read(TCP_CHUNK)
                if not chunk:
                    break
                sock.sendall(chunk)
                sent += len(chunk)
                if sent == size or sent % (256 * 1024) == 0:
                    dt = max(time.monotonic() - start, 0.001)
                    print(f"TCP progress: {sent}/{size} {sent / 1024.0 / dt:.1f} KiB/s")
        line = bytearray()
        while True:
            b = sock.recv(1)
            if not b or b == b"\n":
                break
            if b != b"\r":
                line += b
    elapsed = max(time.monotonic() - start, 0.001)
    response = line.decode("utf-8", errors="replace")
    print(f"TCP response: {response or '<empty>'} ({elapsed:.2f}s, {size / 1024.0 / elapsed:.1f} KiB/s)")
    if not response.startswith("OK"):
        raise SystemExit(f"TCP PUT failed: {response}")
    return elapsed


class KsdClient:
    def __init__(self, port: str, baud: int, timeout: float):
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SystemExit("pyserial is not installed. Run: py -3 -m pip install -r requirements.txt") from exc
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.port = port
        self.baud = baud
        self.timeout = timeout

    def close(self) -> None:
        self.ser.close()

    def read_line_once(self) -> str | None:
        raw = self.ser.readline()
        if not raw:
            return None
        return raw.decode("utf-8", errors="replace").rstrip()

    def wait_ksd(self, prefixes: tuple[str, ...], stage: str, timeout: float | None = None) -> str:
        deadline = time.monotonic() + (timeout or self.timeout)
        while time.monotonic() < deadline:
            line = self.read_line_once()
            if not line:
                continue
            if line.startswith("KSD:"):
                print(f"KSD: {line}")
                if line.startswith(prefixes):
                    return line
            else:
                print(f"K210: {line}")
        raise TimeoutError(stage)

    def connect(self) -> None:
        print(f"KSD connect: {self.port} @ {self.baud}")
        deadline = time.monotonic() + self.timeout
        next_magic = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_magic:
                self.ser.write(KSD_MAGIC)
                self.ser.flush()
                next_magic = now + 0.25
            line = self.read_line_once()
            if not line:
                continue
            if line.startswith("KSD:"):
                print(f"KSD: {line}")
                if line.startswith("KSD:READY"):
                    self.ser.write(KSD_MAGIC)
                    self.ser.flush()
                if line.startswith("KSD:HELLO"):
                    print("KSD connected")
                    return
            else:
                print(f"K210: {line}")
        raise TimeoutError("KSD handshake timeout")

    def command_prompt(self) -> None:
        self.wait_ksd(("KSD:CMD",), "KSD command prompt")

    def run_spi(self) -> None:
        self.command_prompt()
        print("KSD RUN_SPI")
        self.ser.write(b"RUN_SPI\n")
        self.ser.flush()
        self.wait_ksd(("KSD:RUNSPI", "KSD:ERR"), "RUN_SPI")

    def get_file(self, remote: str, dst: Path) -> float:
        self.command_prompt()
        print(f"KSD GET: {remote}")
        self.ser.write(f"GET {remote}\n".encode("ascii"))
        self.ser.flush()
        line = self.wait_ksd(("KSD:SIZE", "KSD:MISSING", "KSD:ERR"), f"GET {remote} size", timeout=10.0)
        if line.startswith("KSD:MISSING"):
            raise SystemExit(f"KSD GET failed: {remote} is missing on SD")
        if line.startswith("KSD:ERR"):
            raise SystemExit(f"KSD GET failed: {line}")
        size = int(line.split()[1])
        dst.parent.mkdir(parents=True, exist_ok=True)
        start = time.monotonic()
        got = 0
        with dst.open("wb") as f:
            while got < size:
                chunk = self.ser.read(min(4096, size - got))
                if not chunk:
                    if time.monotonic() - start > self.timeout:
                        raise TimeoutError(f"KSD raw read timeout at {got}/{size}")
                    continue
                f.write(chunk)
                got += len(chunk)
                if got == size or got % (256 * 1024) == 0:
                    dt = max(time.monotonic() - start, 0.001)
                    print(f"KSD GET progress: {got}/{size} {got / 1024.0 / dt:.1f} KiB/s")
        self.wait_ksd(("KSD:OK", "KSD:ERR"), f"GET {remote} final", timeout=10.0)
        elapsed = max(time.monotonic() - start, 0.001)
        print(f"KSD GET done: {dst} ({elapsed:.2f}s, {size / 1024.0 / elapsed:.1f} KiB/s)")
        return elapsed

    def done(self) -> None:
        self.command_prompt()
        self.ser.write(b"DONE\n")
        self.ser.flush()
        self.wait_ksd(("KSD:DONE",), "DONE")


def ksd_run_spi_once(port: str, baud: int, timeout: float) -> None:
    ksd = KsdClient(port, baud, timeout)
    try:
        ksd.connect()
        ksd.run_spi()
        ksd.done()
    finally:
        ksd.close()


def ksd_get_verify(port: str, baud: int, timeout: float, remote: str, dst: Path) -> float:
    ksd = KsdClient(port, baud, timeout)
    try:
        ksd.connect()
        elapsed = ksd.get_file(remote, dst)
        ksd.done()
        return elapsed
    finally:
        ksd.close()


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description="PC -> WiFi -> ESP SPI -> K210 SD write, then KSD SD readback verify")
    ap.add_argument("--host", default="192.168.0.132", help="ESP STA IP; fallback AP is usually 192.168.4.1")
    ap.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    ap.add_argument("--sd-uart", default="COM12")
    ap.add_argument("--sd-baud", type=int, default=DEFAULT_KSD_BAUD)
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE)
    ap.add_argument("--remote", default="t1m.bin")
    ap.add_argument("--timeout", type=float, default=45.0)
    args = ap.parse_args()

    out = root / "out" / "wifi_spi_sd_test"
    local = out / f"test_{args.size}.bin"
    readback = out / f"readback_{args.remote}"

    print("=== PC -> WiFi -> SPI -> SD fast RW test ===")
    print(f"ESP host: {args.host}:{args.tcp_port}")
    print(f"KSD UART: {args.sd_uart} @ {args.sd_baud}")
    print(f"Remote SD name: {args.remote}")
    print(f"Size: {args.size} bytes")

    make_test_file(local, args.size)
    src_hash = sha256_file(local)
    print(f"Local SHA256: {src_hash}")

    print("Start K210 ESP UART/SPI receiver through KSD RUN_SPI...")
    ksd_run_spi_once(args.sd_uart, args.sd_baud, args.timeout)
    time.sleep(1.0)

    tcp_s = tcp_put(args.host, args.tcp_port, local, args.remote, args.timeout)

    print("Wait 2 seconds for K210 SD close/log flush...")
    time.sleep(2.0)

    get_s = ksd_get_verify(args.sd_uart, args.sd_baud, args.timeout, args.remote, readback)

    rb_hash = sha256_file(readback)
    print(f"Readback SHA256: {rb_hash}")
    if rb_hash != src_hash:
        raise SystemExit("VERIFY FAIL: SHA256 mismatch")

    print("VERIFY OK: readback matches original")
    print(f"WRITE TCP speed: {args.size / 1024.0 / max(tcp_s, 0.001):.1f} KiB/s")
    print(f"READ KSD speed:  {args.size / 1024.0 / max(get_s, 0.001):.1f} KiB/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
