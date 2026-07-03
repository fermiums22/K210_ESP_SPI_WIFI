#!/usr/bin/env python3
"""Automatic K210 SD UART upload path.

This wrapper reuses the existing ESP payload builder, but the K210 UART path is
fully automatic: PC opens COM, pulses UART control lines, performs KSD handshake,
writes payload files, updates flash.json, sends RESET, and monitors ESP result.
"""
from __future__ import annotations

import argparse
import json
import logging
import sys
import time
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import send_flash_payload as base  # noqa: E402

# Must match K210_AI_V7s_Plus/src/sd_uart.c UART_SD_BUF.
# 512-byte bursts were not stable on this UARTHS+SD path and could stop around 128 KiB.
KSD_UPLOAD_CHUNK = 128


class KsdAutoClient:
    def __init__(self, port: str, baud: int, reset_mode: str):
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SystemExit("pyserial is not installed. Run: py -3 -m pip install -r requirements.txt") from exc
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.port = port
        self.baud = baud
        self.reset_mode = reset_mode.lower()

    def close(self) -> None:
        self.ser.close()

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def write_command(self, line: str) -> None:
        self.write(line.encode("ascii"))

    def write_payload_block(self, data: bytes) -> None:
        self.write(data)

    def set_lines(self, dtr: bool, rts: bool, delay_s: float) -> None:
        self.ser.dtr = dtr
        self.ser.rts = rts
        time.sleep(delay_s)

    def pulse_line(self, line_name: str) -> None:
        if line_name == "rts":
            logging.info("auto-reset: pulse RTS")
            self.set_lines(False, True, 0.12)
            self.set_lines(False, False, 0.25)
        elif line_name == "dtr":
            logging.info("auto-reset: pulse DTR")
            self.set_lines(True, False, 0.12)
            self.set_lines(False, False, 0.25)
        elif line_name == "both":
            logging.info("auto-reset: pulse DTR+RTS")
            self.set_lines(True, True, 0.12)
            self.set_lines(False, False, 0.25)
        else:
            raise ValueError(line_name)

    def auto_reset(self) -> None:
        logging.info("auto-reset mode: %s", self.reset_mode)
        self.set_lines(False, False, 0.05)
        if self.reset_mode == "none":
            return
        if self.reset_mode == "rts":
            self.pulse_line("rts")
        elif self.reset_mode == "dtr":
            self.pulse_line("dtr")
        elif self.reset_mode == "both":
            self.pulse_line("both")
        elif self.reset_mode == "dan":
            self.pulse_line("rts")
            self.pulse_line("dtr")
            self.set_lines(False, False, 0.35)
        else:
            raise SystemExit(f"Unknown auto-reset mode: {self.reset_mode}")

    def read_line_once(self, *, quiet_ksd_block_ack: bool = True) -> str | None:
        raw = self.ser.readline()
        if not raw:
            return None
        line = raw.decode("utf-8", errors="replace").rstrip()
        if line and not (quiet_ksd_block_ack and line == "KSD:B"):
            logging.info("K210: %s", line)
        return line

    def stage_line(self, prefixes: tuple[str, ...], stage: str | None = None) -> str:
        if stage:
            logging.info("stage: %s", stage)
        while True:
            line = self.read_line_once()
            if not line or not line.startswith("KSD:"):
                continue
            if line.startswith(prefixes):
                return line

    def connect(self) -> None:
        logging.info("sd-uart: opening %s @ %d", self.port, self.baud)
        logging.info("stage: automatic K210 reset")
        self.auto_reset()
        logging.info("stage: KSD handshake; no SD write and no ESP flash yet")
        next_magic = 0.0
        while True:
            now = time.monotonic()
            if now >= next_magic:
                self.write(base.KSD_MAGIC)
                next_magic = now + 0.25
            line = self.read_line_once()
            if not line or not line.startswith("KSD:"):
                continue
            if line.startswith("KSD:READY"):
                logging.info("sd-uart: KSD:READY seen; sending HELLO magic")
                self.write(base.KSD_MAGIC)
                next_magic = time.monotonic() + 0.25
                continue
            if line.startswith("KSD:HELLO"):
                logging.info("sd-uart: connected")
                return

    def command_prompt(self) -> None:
        self.stage_line(("KSD:CMD",), "K210 command prompt")

    def read_exact_loop(self, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            chunk = self.ser.read(size - len(data))
            if chunk:
                data.extend(chunk)
        return bytes(data)

    def get_file(self, name: str) -> str | None:
        self.command_prompt()
        logging.info("sd-uart GET %s", name)
        self.write_command(f"GET {name}\n")
        line = self.stage_line(("KSD:SIZE", "KSD:MISSING", "KSD:ERR"), f"GET {name} response")
        if line.startswith("KSD:MISSING"):
            logging.info("sd-uart GET %s: missing", name)
            return None
        if line.startswith("KSD:ERR"):
            raise SystemExit(f"K210 GET failed: {line}")
        size = int(line.split()[1])
        data = self.read_exact_loop(size)
        ok = self.stage_line(("KSD:OK", "KSD:ERR"), f"GET {name} final ACK")
        if not ok.startswith("KSD:OK"):
            raise SystemExit(f"K210 GET failed after data: {ok}")
        text = data.decode("utf-8", errors="replace")
        logging.info("sd-uart GET %s: %d bytes", name, size)
        logging.info("sd-uart existing %s: %s", name, text.replace("\n", " "))
        return text

    def put_file(self, path: Path, remote_name: str) -> None:
        size = path.stat().st_size
        self.command_prompt()
        logging.info("sd-uart PUT %s (%d bytes, chunk=%d)", remote_name, size, KSD_UPLOAD_CHUNK)
        self.write_command(f"PUT {remote_name} {size}\n")
        line = self.stage_line(("KSD:GO", "KSD:ERR"), f"PUT {remote_name} GO")
        if not line.startswith("KSD:GO"):
            raise SystemExit(f"K210 refused PUT {remote_name}: {line}")
        line = self.stage_line(("KSD:READYDATA", "KSD:ERR"), f"PUT {remote_name} data-ready")
        if not line.startswith("KSD:READYDATA"):
            raise SystemExit(f"K210 PUT {remote_name} did not enter data mode: {line}")

        sent = 0
        with path.open("rb") as f:
            while sent < size:
                chunk = f.read(KSD_UPLOAD_CHUNK)
                if not chunk:
                    break
                self.write_payload_block(chunk)
                sent += len(chunk)
                line = self.stage_line(("KSD:B", "KSD:ERR"))
                if line.startswith("KSD:ERR"):
                    raise SystemExit(f"K210 PUT failed at {sent}/{size}: {line}")
                if sent == size or sent % 32768 == 0:
                    logging.info("sd-uart progress: %s %d/%d", remote_name, sent, size)
        ok = self.stage_line(("KSD:OK", "KSD:ERR"), f"PUT {remote_name} final ACK")
        if not ok.startswith("KSD:OK"):
            raise SystemExit(f"K210 PUT failed after data: {ok}")
        logging.info("sd-uart PUT done: %s", remote_name)

    def reset_and_monitor(self) -> None:
        self.command_prompt()
        logging.info("sd-uart RESET")
        self.write_command("RESET\n")
        self.stage_line(("KSD:RESETTING", "KSD:ERR"), "RESET ACK")
        logging.info("stage: K210 rebooted; monitoring ESP boot result")
        success_markers = (
            "ESP flash result: OK",
            "[esp-flash] done",
            "kesp: version=",
            "kesp: wifi begin",
        )
        hard_fail_markers = (
            "ESP flash result: FAIL",
            "[esp-flash] connect failed",
            "[esp-flash] image missing",
            "[esp-flash] bad image size",
        )
        soft_fail_markers = (
            "[esp-flash] finish failed",
        )
        deadline = time.monotonic() + 180.0
        saw_soft_fail = False
        while time.monotonic() < deadline:
            line = self.read_line_once()
            if not line:
                continue
            if any(marker in line for marker in success_markers):
                if saw_soft_fail:
                    logging.warning("sd-uart monitor: ESP booted after flash finish warning")
                else:
                    logging.info("sd-uart monitor: ESP boot marker found")
                return
            if any(marker in line for marker in hard_fail_markers):
                raise SystemExit(f"K210 ESP flash failed: {line}")
            if any(marker in line for marker in soft_fail_markers):
                saw_soft_fail = True
                logging.warning("sd-uart monitor: %s; waiting for ESP boot log", line)
        raise SystemExit("K210 ESP monitor timeout: no kesp boot/version log")


def upload_sd_uart(port: str, baud: int, parts: list[base.FlashPart], reset_mode: str) -> None:
    client = KsdAutoClient(port, baud, reset_mode)
    try:
        client.connect()
        existing = client.get_file("flash.json")
        cfg = base.patch_flash_config(existing, parts)
        files = base.write_payload(parts, cfg)
        ordered = [p for p in files if p.name != "flash.json"] + [p for p in files if p.name == "flash.json"]

        logging.info("planned SD writes:")
        for path in ordered:
            logging.info("  PUT %s (%d bytes)", path.name, path.stat().st_size)
        logging.info("new flash_once section:")
        logging.info(json.dumps(cfg.get("flash_once", {}), indent=2))

        for path in ordered:
            client.put_file(path, path.name)
        client.reset_and_monitor()
    finally:
        client.close()


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Automatic K210 SD UART upload of ESP8285 payload")
    ap.add_argument("--sd-uart", required=True, help="K210 debug UART COM port, e.g. COM12")
    ap.add_argument("--sd-baud", type=int, default=115200, help="K210 debug UART baud")
    ap.add_argument("--auto-reset", default="dan", choices=("dan", "rts", "dtr", "both", "none"))
    ap.add_argument("--env", default="esp8285")
    ap.add_argument("--build-dir")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--firmware")
    ap.add_argument("--remote-name")
    ap.add_argument("--offset", default="0x0")
    ap.add_argument("--full-flash", action="store_true", help="Legacy no-op: app-only firmware.bin is now the default")
    ap.add_argument("--force-full-flash", action="store_true", help="Actually upload and flash the 1 MB merged image")
    return ap.parse_args()


def main() -> int:
    log_path = base.setup_logging()
    args = parse_args()
    logging.info("repo: %s", base.ROOT)
    logging.info("log: %s", log_path)
    logging.info("mode: automatic SD UART upload")

    if args.full_flash and not args.force_full_flash:
        logging.warning("--full-flash is legacy/no-op now; using fast app-only firmware.bin. Use --force-full-flash only when a real 1 MB rewrite is needed.")
    args.full_flash = bool(args.force_full_flash)

    if not args.no_build and not args.firmware:
        base.build_platformio(args.env)
    else:
        logging.info("build skipped")

    parts = base.collect_parts(args)
    upload_sd_uart(args.sd_uart, args.sd_baud, parts, args.auto_reset)
    logging.info("done. Send this log if flashing fails: %s", log_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
