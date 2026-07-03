#!/usr/bin/env python3
from __future__ import annotations

import argparse
import socket
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


DEFAULT_SERIAL_PORT = "COM12"
DEFAULT_HOST = "192.168.0.132"
DEFAULT_TCP_PORT = 18080
DEFAULT_TEST_SIZE = 1024 * 1024
DEFAULT_LOCAL_FILE = "test_1m.bin"
DEFAULT_REMOTE_NAME = "t1m.bin"
SEND_CHUNK = 16 * 1024
PROGRESS_STEP = 256 * 1024


class SocketAccessDenied(RuntimeError):
    pass


@dataclass
class TcpPutResult:
    response: str
    sent: int
    elapsed_s: float


class SerialCapture:
    def __init__(self, port: str, baud: int, log_path: Path, echo: bool = True) -> None:
        self.port = port
        self.baud = baud
        self.log_path = log_path
        self.echo = echo
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._lines: list[str] = []
        self._text_tail = ""
        self._thread: threading.Thread | None = None
        self._ser = None
        self._log_file = None
        self._partial = ""

    def start(self) -> None:
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise RuntimeError("pyserial is not installed. Run: py -3 -m pip install pyserial") from exc

        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log_file = self.log_path.open("w", encoding="utf-8", errors="replace")
        self._log_file.write(f"# Serial capture {self.port} @ {self.baud}\n")
        self._log_file.flush()

        ser = serial.Serial()
        ser.port = self.port
        ser.baudrate = self.baud
        ser.bytesize = 8
        ser.parity = "N"
        ser.stopbits = 1
        ser.timeout = 0.05
        ser.write_timeout = 0.05
        ser.xonxoff = False
        ser.rtscts = False
        ser.dsrdtr = False
        # Do not reset or hold K210 boot pins through CH340/auto-boot wiring.
        ser.dtr = False
        ser.rts = False
        ser.open()
        ser.dtr = False
        ser.rts = False
        self._ser = ser

        self._thread = threading.Thread(target=self._reader, name="k210-serial-capture", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        if self._ser is not None:
            try:
                self._ser.dtr = False
                self._ser.rts = False
            except Exception:
                pass
            try:
                self._ser.close()
            except Exception:
                pass
        if self._log_file is not None:
            self._log_file.flush()
            self._log_file.close()

    def contains(self, needle: str) -> bool:
        with self._lock:
            if needle in self._text_tail:
                return True
            return any(needle in line for line in self._lines[-500:])

    def wait_contains_any(self, needles: list[str], timeout_s: float) -> str | None:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for needle in needles:
                if self.contains(needle):
                    return needle
            time.sleep(0.05)
        return None

    def tail(self, max_lines: int = 40) -> str:
        with self._lock:
            return "\n".join(self._lines[-max_lines:])

    def _reader(self) -> None:
        assert self._ser is not None
        while not self._stop.is_set():
            try:
                data = self._ser.read(4096)
            except Exception as exc:
                self._append_line(f"[serial] read error: {exc}", force_echo=True)
                break
            if not data:
                time.sleep(0.01)
                continue
            text = data.decode("utf-8", errors="replace")
            if self._log_file is not None:
                self._log_file.write(text)
                self._log_file.flush()
            self._feed_text(text)

    def _feed_text(self, text: str) -> None:
        with self._lock:
            self._text_tail = (self._text_tail + text)[-16384:]
        text = self._partial + text
        parts = text.splitlines(keepends=True)
        self._partial = ""
        for part in parts:
            if part.endswith("\n") or part.endswith("\r"):
                line = part.strip("\r\n")
                self._append_line(line)
            else:
                self._partial = part

    def _append_line(self, line: str, force_echo: bool = False) -> None:
        if not line:
            return
        with self._lock:
            self._lines.append(line)
            if len(self._lines) > 2000:
                del self._lines[:500]
        if force_echo or (self.echo and is_interesting_serial_line(line)):
            print(line, flush=True)


def is_interesting_serial_line(line: str) -> bool:
    tokens = (
        "kesp: boot",
        "kesp: version",
        "kesp: wifi begin",
        "kesp: wifi connected",
        "kesp: PUT",
        "kesp: DONE",
        "[esp-uart]",
        "[wifi-spi] ready",
        "[wifi-spi] ESP SPI ready",
        "[wifi-spi] idle",
        "[wifi-spi] TCP PUT",
        "[wifi-spi] BEGIN",
        "[wifi-spi] END",
        "[wifi-spi] bad",
        "Exception",
        "exception",
        "panic",
        "wdt",
    )
    return any(token in line for token in tokens)


def ensure_test_file(path: Path, size: int) -> None:
    if path.exists() and path.is_file() and path.stat().st_size == size:
        print(f"test file: {path} already exists ({size} bytes)")
        return
    print(f"test file: creating {path} ({size} bytes)")
    path.parent.mkdir(parents=True, exist_ok=True)
    block = bytes((i & 0xFF for i in range(4096)))
    left = size
    with path.open("wb") as f:
        while left > 0:
            chunk = block if left >= len(block) else block[:left]
            f.write(chunk)
            left -= len(chunk)


def recv_line(sock: socket.socket, timeout_s: float) -> str:
    sock.settimeout(timeout_s)
    data = bytearray()
    while True:
        b = sock.recv(1)
        if not b:
            break
        if b == b"\n":
            break
        if b != b"\r":
            data += b
    return data.decode("utf-8", errors="replace").strip()


def tcp_put_file(host: str, port: int, path: Path, remote_name: str, timeout_s: float) -> TcpPutResult:
    size = path.stat().st_size
    print(f"TCP PUT {path} -> {host}:{port}/{remote_name} ({size} bytes)")
    start = time.monotonic()
    try:
        sock_cm = socket.create_connection((host, port), timeout=timeout_s)
    except PermissionError as exc:
        raise SocketAccessDenied(str(exc)) from exc
    except OSError as exc:
        if getattr(exc, "winerror", None) == 10013:
            raise SocketAccessDenied(str(exc)) from exc
        raise

    with sock_cm as sock:
        sock.settimeout(timeout_s)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(f"PUT {remote_name} {size}\n".encode("ascii"))
        sent = 0
        next_report = PROGRESS_STEP
        with path.open("rb") as f:
            while True:
                chunk = f.read(SEND_CHUNK)
                if not chunk:
                    break
                sock.sendall(chunk)
                sent += len(chunk)
                if sent >= next_report or sent == size:
                    elapsed = max(time.monotonic() - start, 0.001)
                    print(f"tcp progress {sent}/{size} {sent / 1024.0 / elapsed:.1f} KiB/s")
                    next_report += PROGRESS_STEP
        response = recv_line(sock, timeout_s)
    elapsed = time.monotonic() - start
    print(f"tcp response: {response or '<empty>'} ({elapsed:.1f}s)")
    return TcpPutResult(response=response, sent=size, elapsed_s=elapsed)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="One-command PC -> Wi-Fi -> ESP8285 -> SPI -> K210 -> SD smoke test"
    )
    ap.add_argument("serial_port", nargs="?", default=DEFAULT_SERIAL_PORT, help="K210 UART COM port")
    ap.add_argument("host", nargs="?", default=DEFAULT_HOST, help="ESP STA IP or fallback AP IP")
    ap.add_argument("local_file", nargs="?", default=DEFAULT_LOCAL_FILE, help="Local test file")
    ap.add_argument("remote_name", nargs="?", default=DEFAULT_REMOTE_NAME, help="Remote SD file name")
    ap.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--size", type=int, default=DEFAULT_TEST_SIZE)
    ap.add_argument("--connect-timeout", type=float, default=8.0)
    ap.add_argument("--spi-timeout", type=float, default=20.0)
    ap.add_argument("--settle", type=float, default=1.0, help="Seconds to collect UART before TCP PUT")
    ap.add_argument("--no-serial", action="store_true", help="Run TCP PUT without K210 UART capture")
    ap.add_argument("--quiet-serial", action="store_true", help="Write full serial log but print only test summary")
    return ap.parse_args()


def main() -> int:
    args = parse_args()
    repo = Path(__file__).resolve().parents[1]
    local_file = Path(args.local_file)
    if not local_file.is_absolute():
        local_file = repo / local_file
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = repo / "logs" / f"wifi_sd_smoke_{timestamp}.log"

    print("=== WiFi/SPI/SD smoke test ===")
    print(f"repo:        {repo}")
    print(f"serial:      {args.serial_port} @ {args.baud}")
    print(f"target:      {args.host}:{args.tcp_port}")
    print(f"local file:  {local_file}")
    print(f"remote name: {args.remote_name}")
    print(f"serial log:  {log_path}")
    print("flash:       skipped; this test uses current K210/ESP firmware")

    ensure_test_file(local_file, args.size)

    capture: SerialCapture | None = None
    if not args.no_serial:
        capture = SerialCapture(args.serial_port, args.baud, log_path, echo=not args.quiet_serial)
        try:
            capture.start()
        except Exception as exc:
            print(f"RESULT: FAIL_SERIAL_OPEN")
            print(f"Cannot open {args.serial_port}: {exc}")
            print("Close any other monitor/terminal that owns the COM port and rerun the same command.")
            return 20
        print("serial capture: started without DTR/RTS reset")
        time.sleep(max(args.settle, 0.0))
    else:
        print("serial capture: disabled")

    try:
        try:
            result = tcp_put_file(args.host, args.tcp_port, local_file, args.remote_name, args.connect_timeout)
        except SocketAccessDenied as exc:
            time.sleep(1.0)
            print("RESULT: FAIL_SOCKET_ACCESS_DENIED")
            print(f"Windows rejected TCP connect to {args.host}:{args.tcp_port} before ESP received PUT.")
            print(f"socket error: {exc}")
            if capture is not None:
                print(f"serial log: {capture.log_path}")
            return 11
        except OSError as exc:
            time.sleep(1.0)
            print("RESULT: FAIL_TCP_CONNECT")
            print(f"TCP connect/send failed for {args.host}:{args.tcp_port}: {exc}")
            if capture is not None:
                print(f"serial log: {capture.log_path}")
            return 12

        if not result.response.startswith("OK"):
            print("RESULT: FAIL_TCP_RESPONSE")
            print(f"ESP response was not OK: {result.response or '<empty>'}")
            if capture is not None:
                print(f"serial log: {capture.log_path}")
            return 13

        if capture is None:
            print("RESULT: OK_TCP_ONLY")
            print("TCP PUT returned OK, but serial capture was disabled, so SPI/SD END was not verified.")
            return 0

        size = local_file.stat().st_size
        end_needles = [
            f"[wifi-spi] END {args.remote_name} {size}/{size}",
            f"[wifi-spi] END {args.remote_name}",
            "[wifi-spi] END",
        ]
        matched = capture.wait_contains_any(end_needles, args.spi_timeout)
        if matched:
            print("RESULT: OK_WIFI_SPI_SD")
            print(f"matched: {matched}")
            print(f"serial log: {capture.log_path}")
            return 0

        print("RESULT: FAIL_SPI_NO_END")
        print(f"TCP PUT returned OK, but K210 did not report [wifi-spi] END within {args.spi_timeout:.1f}s.")
        print(f"serial log: {capture.log_path}")
        print("last serial lines:")
        print(capture.tail(30))
        return 14
    finally:
        if capture is not None:
            capture.stop()


if __name__ == "__main__":
    raise SystemExit(main())
