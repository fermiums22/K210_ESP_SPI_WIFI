#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
import time

try:
    import serial
except Exception as exc:
    print(f"ERROR: pyserial import failed: {exc}")
    print("Run: py -3 -m pip install pyserial")
    raise SystemExit(2)

KSD_DEFAULT_BAUD = 921600


def open_port(port: str, baud: int) -> serial.Serial:
    deadline = time.monotonic() + 10.0
    last: Exception | None = None
    while time.monotonic() < deadline:
        try:
            ser = serial.Serial(port=port, baudrate=baud, timeout=0.05)
            try:
                ser.dtr = False
                ser.rts = False
            except Exception:
                pass
            return ser
        except Exception as exc:
            last = exc
            time.sleep(0.2)
    raise SystemExit(f"ERROR: cannot open {port}: {last}")


def read_line(ser: serial.Serial, timeout: float) -> str | None:
    deadline = time.monotonic() + timeout
    buf = bytearray()
    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        if b == b"\n":
            return bytes(buf).decode("latin1", errors="replace").strip()
    return None


def connect_ksd(ser: serial.Serial, timeout_s: float) -> None:
    ser.reset_input_buffer()
    deadline = time.monotonic() + timeout_s
    next_magic = 0.0
    print(f"[ksd] connecting for up to {timeout_s:.1f}s; reset K210 if it is not in persistent service")
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_magic:
            ser.write(b"KSD1\n")
            ser.flush()
            next_magic = now + 0.25
        line = read_line(ser, 0.25)
        if not line:
            continue
        print(line)
        if "KSD:HELLO" in line:
            print("[ksd] connected")
            return
    raise SystemExit("ERROR: KSD connect timeout; press RESET on K210 and rerun")


def wait_cmd_prompt(ser: serial.Serial, timeout_s: float) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = read_line(ser, 0.5)
        if not line:
            continue
        print(line)
        if "KSD:CMD" in line:
            return
    raise SystemExit("ERROR: KSD command prompt timeout")


def parse_good_bad(line: str) -> tuple[int, int] | None:
    mg = re.search(r"good=(\d+)", line)
    mb = re.search(r"bad=(\d+)", line)
    if not mg or not mb:
        return None
    return int(mg.group(1)), int(mb.group(1))


def run_spi_ping(ser: serial.Serial, timeout_s: float) -> int:
    wait_cmd_prompt(ser, 10.0)
    print("[ksd] cmd: RUN_SPI")
    ser.write(b"RUN_SPI\n")
    ser.flush()

    deadline = time.monotonic() + timeout_s
    ack = False
    ready = False
    best_line = ""

    while time.monotonic() < deadline:
        line = read_line(ser, 0.5)
        if not line:
            continue
        print(line)

        if "KSD:RUNSPI" in line:
            ack = True
        if "kesp: spi slave ready" in line:
            ready = True
        if "[pure-spi] best " in line:
            best_line = line
            gb = parse_good_bad(line)
            if gb:
                good, bad = gb
                if good > 0:
                    print(f"OK: SPI ping detected KESP frame: good={good} bad={bad}")
                    return 0

        if "[pure-spi] open /dev/spi0 failed" in line:
            raise SystemExit("ERROR: K210 could not open /dev/spi0")
        if "kesp: spi slave init failed" in line:
            raise SystemExit("ERROR: ESP8285 SPI slave init failed")

    raise SystemExit(
        "ERROR: SPI ping timeout; "
        f"ack={ack} ready={ready} last_best={best_line or 'none'}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Run K210 KSD RUN_SPI and wait for ESP8285 KESP SPI ping")
    ap.add_argument("--sd-uart", required=True, help="K210 debug/service UART, e.g. COM12")
    ap.add_argument("--sd-baud", type=int, default=KSD_DEFAULT_BAUD)
    ap.add_argument("--connect-timeout", type=float, default=25.0)
    ap.add_argument("--timeout", type=float, default=45.0)
    args = ap.parse_args()

    ser = open_port(args.sd_uart, args.sd_baud)
    try:
        connect_ksd(ser, args.connect_timeout)
        return run_spi_ping(ser, args.timeout)
    finally:
        ser.close()


if __name__ == "__main__":
    raise SystemExit(main())
