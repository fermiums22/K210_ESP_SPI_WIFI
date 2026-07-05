#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import logging
import re
import socket
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
KSD_MAGIC = b"KSD1\n"
DEFAULT_REMOTE_NAME = "pc_wifi_spi_sd.bin"


def setup_logging() -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"pc_wifi_spi_sd_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s: %(message)s",
        datefmt="%H:%M:%S",
        handlers=[logging.StreamHandler(sys.stdout), logging.FileHandler(log_path, encoding="utf-8")],
    )
    return log_path


def make_payload(size: int) -> bytes:
    seed = b"K210-ESP8285-PC-WIFI-SPI-SD\n"
    out = bytearray()
    counter = 0
    while len(out) < size:
        h = hashlib.sha256(seed + counter.to_bytes(4, "little")).digest()
        out.extend(h)
        counter += 1
    return bytes(out[:size])


class LineGuard:
    def __init__(self, max_lines: int, stage: str):
        self.max_lines = max_lines
        self.stage = stage
        self.count = 0

    def tick(self) -> None:
        self.count += 1
        if self.count > self.max_lines:
            raise SystemExit(f"{self.stage}: line guard exceeded ({self.max_lines}); no automatic retry")


class Ksd:
    def __init__(self, port: str, baud: int, max_lines: int):
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SystemExit("pyserial is missing; install requirements.txt") from exc
        # Finite timeout is used only for initial boot-banner drain. Once KSD is
        # connected, reads become blocking so normal quiet periods do not look
        # like protocol failures.
        self.ser = serial.Serial(port, baud, timeout=1.0, write_timeout=5.0)
        self.ser.dtr = False
        self.ser.rts = False
        self.max_lines = max_lines
        self.have_prompt = False

    def close(self) -> None:
        self.ser.close()

    def line(self, stage: str) -> str:
        raw = self.ser.readline()
        if not raw:
            raise SystemExit(f"{stage}: no line from K210")
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        logging.info("K210: %s", line)
        return line

    def connect(self) -> None:
        logging.info("KSD connect: drain boot banner, send one magic after listener/quiet")
        for _ in range(self.max_lines):
            raw = self.ser.readline()
            if not raw:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            logging.info("K210: %s", line)
            if line.startswith("KSD:CMD"):
                self.have_prompt = True
                self.ser.timeout = None
                return
            if line.startswith("KSD:HELLO"):
                self.ser.timeout = None
                return
            if line.startswith("KSD:READY") or "PC UART KSD listener" in line:
                break
        self.ser.write(KSD_MAGIC)
        self.ser.flush()
        guard = LineGuard(self.max_lines, "connect")
        while True:
            guard.tick()
            line = self.line("connect")
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"connect: {line}")
            if line.startswith("KSD:HELLO"):
                self.ser.timeout = None
                return
            if line.startswith("KSD:CMD"):
                self.have_prompt = True
                self.ser.timeout = None
                return

    def wait_prompt(self, stage: str) -> None:
        if self.have_prompt:
            self.have_prompt = False
            return
        guard = LineGuard(self.max_lines, stage)
        while True:
            guard.tick()
            line = self.line(stage)
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"{stage}: {line}")
            if line.startswith("KSD:CMD"):
                return

    def send_command(self, cmd: str, stage: str) -> None:
        self.wait_prompt(stage + " prompt")
        logging.info("KSD > %s", cmd)
        self.ser.write((cmd + "\n").encode("ascii"))
        self.ser.flush()

    def start_run_spi_and_wait_ready(self) -> tuple[str, int]:
        self.send_command("RUN_SPI", "RUN_SPI")
        guard = LineGuard(self.max_lines, "RUN_SPI readiness")
        ip = ""
        fallback_ip = ""
        port = 18080
        tcp_ready = False
        spi_ok = False
        while True:
            guard.tick()
            line = self.line("RUN_SPI readiness")
            if line.startswith("KSD:CMD"):
                self.have_prompt = True
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"RUN_SPI: {line}")
            m = re.search(r"kesp: wifi connected ip=([0-9.]+)", line)
            if m:
                ip = m.group(1)
                logging.info("ESP STA IP detected: %s", ip)
            m = re.search(r"fallback AP .*ip=([0-9.]+)", line)
            if m:
                fallback_ip = m.group(1)
                logging.info("ESP fallback AP IP detected: %s", fallback_ip)
            m = re.search(r"kesp: tcp server ready port=(\d+)", line)
            if m:
                port = int(m.group(1))
                tcp_ready = True
            if "[wifi-sd] VERDICT SPI_OK" in line:
                spi_ok = True
            if "[wifi-sd] VERDICT SPI_FAIL" in line:
                raise SystemExit("RUN_SPI: K210 did not find KESP SPI frame")
            if tcp_ready and spi_ok and (ip or fallback_ip):
                return (ip or fallback_ip, port)

    def wait_wifi_sd_ok(self, remote_rel: str) -> None:
        guard = LineGuard(self.max_lines, "WIFI_SD_OK")
        token = f"WIFI_SD_OK {remote_rel}"
        while True:
            guard.tick()
            line = self.line("WIFI_SD_OK")
            if line.startswith("KSD:CMD"):
                self.have_prompt = True
            if "WIFI_SD_FAIL" in line or "DATA offset mismatch" in line or "BEGIN sd_mount failed" in line:
                raise SystemExit(f"WiFi->SPI->SD failed: {line}")
            if token in line:
                logging.info("matched %s", token)
                return

    def get_file(self, rel_path: str) -> bytes:
        self.send_command(f"GET {rel_path}", "GET")
        guard = LineGuard(self.max_lines, "GET size")
        size = None
        while size is None:
            guard.tick()
            line = self.line("GET size")
            if line.startswith("KSD:MISSING") or line.startswith("KSD:ERR"):
                raise SystemExit(f"GET {rel_path}: {line}")
            if line.startswith("KSD:SIZE "):
                size = int(line.split()[1])
        data = self.ser.read(size)
        if len(data) != size:
            raise SystemExit(f"GET {rel_path}: short binary read {len(data)}/{size}")
        guard = LineGuard(self.max_lines, "GET final")
        while True:
            guard.tick()
            line = self.line("GET final")
            if line.startswith("KSD:OK"):
                return data
            if line.startswith("KSD:ERR"):
                raise SystemExit(f"GET {rel_path}: {line}")


def tcp_put(host: str, port: int, name: str, payload: bytes) -> str:
    logging.info("TCP connect %s:%d", host, port)
    with socket.create_connection((host, port)) as sock:
        header = f"PUT {name} {len(payload)}\n".encode("ascii")
        logging.info("TCP > %s", header.decode("ascii").rstrip())
        sock.sendall(header)
        sock.sendall(payload)
        chunks = []
        while True:
            b = sock.recv(1024)
            if not b:
                break
            chunks.append(b)
    reply = b"".join(chunks).decode("utf-8", errors="replace").strip()
    logging.info("TCP < %s", reply)
    if not reply.startswith("OK"):
        raise SystemExit(f"TCP PUT failed: {reply}")
    return reply


def main() -> int:
    ap = argparse.ArgumentParser(description="One-pass PC -> WiFi -> ESP SPI -> K210 SD verifier; no automatic retries.")
    ap.add_argument("--sd-uart", default="COM12")
    ap.add_argument("--sd-baud", type=int, default=921600)
    ap.add_argument("--size", type=int, default=64 * 1024)
    ap.add_argument("--name", default=DEFAULT_REMOTE_NAME)
    ap.add_argument("--host", default="", help="Override ESP TCP host. Empty = parse ESP UART log.")
    ap.add_argument("--port", type=int, default=0, help="Override ESP TCP port. 0 = parse ESP UART log/default 18080.")
    ap.add_argument("--max-lines", type=int, default=4000, help="Line guard per stage; this is not a retry loop.")
    args = ap.parse_args()

    if args.size <= 0:
        raise SystemExit("--size must be positive")
    if not re.match(r"^[A-Za-z0-9_.-]+$", args.name):
        raise SystemExit("--name must contain only A-Z a-z 0-9 _ . -")

    log_path = setup_logging()
    logging.info("repo: %s", ROOT)
    logging.info("log: %s", log_path)
    logging.info("test payload size=%d name=%s", args.size, args.name)

    payload = make_payload(args.size)
    sha = hashlib.sha256(payload).hexdigest()
    remote_rel = "wifi/" + args.name

    ksd = Ksd(args.sd_uart, args.sd_baud, args.max_lines)
    try:
        ksd.connect()
        host, port = ksd.start_run_spi_and_wait_ready()
        if args.host:
            host = args.host
        if args.port:
            port = args.port
        tcp_put(host, port, args.name, payload)
        ksd.wait_wifi_sd_ok(remote_rel)
        got = ksd.get_file(remote_rel)
    finally:
        ksd.close()

    got_sha = hashlib.sha256(got).hexdigest()
    if got != payload:
        raise SystemExit(f"VERIFY_FAIL sha_tx={sha} sha_rx={got_sha} len_rx={len(got)}")
    logging.info("PASS PC_WIFI_SPI_SD path=%s size=%d sha256=%s", remote_rel, len(got), sha)
    print(f"PASS PC_WIFI_SPI_SD path={remote_rel} size={len(got)} sha256={sha}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
