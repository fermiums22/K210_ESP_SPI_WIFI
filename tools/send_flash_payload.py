#!/usr/bin/env python3
"""
Build and send ESP8285 flash payload through the running K210/ESP WiFi-SPI bridge.

Flow:
  PC -> TCP PUT files to ESP8285 bridge on port 7777
  ESP8285 -> SPI frames to K210
  K210 -> writes files to SD
  K210 reboot -> flash.json arms one-shot ESP flashing via K210 esp_flasher
"""
from __future__ import annotations

import argparse
import json
import logging
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
OUT_DIR = ROOT / "out" / "flash_payload"
DEFAULT_TCP_PORT = 7777


@dataclass(frozen=True)
class FlashPart:
    source: Path
    remote_name: str
    offset: int


def setup_logging() -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"flash_payload_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"

    root_logger = logging.getLogger()
    root_logger.setLevel(logging.INFO)
    root_logger.handlers.clear()

    fmt = logging.Formatter("%(asctime)s %(levelname)s: %(message)s", "%H:%M:%S")
    console = logging.StreamHandler(sys.stdout)
    console.setFormatter(fmt)
    root_logger.addHandler(console)

    file_handler = logging.FileHandler(log_path, encoding="utf-8")
    file_handler.setFormatter(fmt)
    root_logger.addHandler(file_handler)
    return log_path


def run(cmd: list[str], cwd: Path) -> None:
    logging.info("run: %s", " ".join(cmd))
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        logging.info(line.rstrip())
    rc = proc.wait()
    if rc != 0:
        raise SystemExit(f"Command failed with code {rc}: {' '.join(cmd)}")


def platformio_cmd() -> list[str]:
    pio = shutil.which("pio")
    if pio:
        return [pio]
    return [sys.executable, "-m", "platformio"]


def build_platformio(env_name: str) -> None:
    run(platformio_cmd() + ["run", "-e", env_name], ROOT)


def maybe_path(path: str | None) -> Path | None:
    if not path:
        return None
    p = Path(path)
    if not p.is_absolute():
        p = ROOT / p
    return p


def collect_parts(args: argparse.Namespace) -> list[FlashPart]:
    explicit_fw = maybe_path(args.firmware)
    if explicit_fw:
        if not explicit_fw.exists():
            raise SystemExit(f"Firmware file not found: {explicit_fw}")
        return [FlashPart(explicit_fw, args.remote_name or "esp8285_at.bin", int(args.offset, 0))]

    build_dir = maybe_path(args.build_dir) or (ROOT / ".pio" / "build" / args.env)
    boot = build_dir / "firmware.bin"
    irom = build_dir / "firmware.bin.irom0text.bin"
    merged = build_dir / "firmware_1m_merged.bin"

    if boot.exists() and irom.exists():
        parts = [
            FlashPart(boot, "esp_boot.bin", 0x00000000),
            FlashPart(irom, "esp_irom.bin", 0x00020000),
        ]
        for candidate, remote, offset in [
            (build_dir / "esp_init_data_default.bin", "esp_init.bin", 0x000FC000),
            (build_dir / "blank.bin", "esp_blank.bin", 0x000FE000),
        ]:
            if candidate.exists():
                parts.append(FlashPart(candidate, remote, offset))
        return parts

    if merged.exists():
        return [FlashPart(merged, "esp8285_at.bin", 0x00000000)]

    if boot.exists():
        logging.warning("Only firmware.bin was found. Sending it as a single image at 0x00000000.")
        logging.warning("If ESP boots incorrectly, check that firmware.bin.irom0text.bin was generated.")
        return [FlashPart(boot, "esp8285_at.bin", 0x00000000)]

    raise SystemExit(
        "No firmware image found. Run PlatformIO build first or pass --firmware path\\to\\image.bin.\n"
        f"Checked: {boot}, {irom}, {merged}"
    )


def write_payload(parts: Iterable[FlashPart]) -> list[Path]:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    copied: list[Path] = []
    json_parts = []

    for part in parts:
        dst = OUT_DIR / part.remote_name
        shutil.copyfile(part.source, dst)
        copied.append(dst)
        json_parts.append({"file": part.remote_name, "offset": f"0x{part.offset:08x}"})
        logging.info("payload: %s -> %s @ 0x%08X (%d bytes)", part.source, part.remote_name, part.offset, dst.stat().st_size)

    flash_json = {
        "flash_once": {
            "enabled": 1,
            "esp": {
                "enabled": 1,
                "parts": json_parts,
            },
        }
    }
    cfg = OUT_DIR / "flash.json"
    cfg.write_text(json.dumps(flash_json, indent=2) + "\n", encoding="utf-8")
    copied.append(cfg)
    logging.info("payload: generated %s", cfg)
    return copied


def recv_line(sock: socket.socket, timeout_s: float) -> str:
    sock.settimeout(timeout_s)
    data = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
        if chunk == b"\n":
            break
    return data.decode("utf-8", errors="replace").strip()


def send_one(host: str, port: int, path: Path, remote_name: str, timeout_s: float) -> None:
    size = path.stat().st_size
    logging.info("send: %s -> %s:%d as %s (%d bytes)", path, host, port, remote_name, size)
    start = time.monotonic()
    with socket.create_connection((host, port), timeout=timeout_s) as sock:
        sock.settimeout(timeout_s)
        header = f"PUT {remote_name} {size}\n".encode("ascii")
        sock.sendall(header)
        with path.open("rb") as f:
            sent = 0
            while True:
                chunk = f.read(4096)
                if not chunk:
                    break
                sock.sendall(chunk)
                sent += len(chunk)
                if sent == size or sent % 32768 == 0:
                    logging.info("send progress: %s %d/%d", remote_name, sent, size)
        response = recv_line(sock, timeout_s)
    elapsed = time.monotonic() - start
    logging.info("response: %s (%.1f s)", response or "<no response>", elapsed)
    if not response.startswith("OK"):
        raise SystemExit(f"Bridge did not acknowledge {remote_name}: {response!r}")


def upload_payload(host: str, port: int, files: list[Path], timeout_s: float) -> None:
    # flash.json must be last: once K210 reboots, it arms the actual ESP flashing job.
    ordered = [p for p in files if p.name != "flash.json"] + [p for p in files if p.name == "flash.json"]
    for path in ordered:
        send_one(host, port, path, path.name, timeout_s)
        time.sleep(0.5)
    logging.info("upload done. Reboot/power-cycle K210_AI_V7s_Plus to run flash_once.")


def monitor_serial(port: str, baud: int, timeout_s: int) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("pyserial is not installed. Run the BAT helper or install pyserial.") from exc

    logging.info("serial monitor: %s @ %d", port, baud)
    stop_markers = ("ESP flash result: OK", "ESP flash result: FAIL", "[esp-flash] done", "[esp-flash] connect failed")
    deadline = time.monotonic() + timeout_s
    with serial.Serial(port, baud, timeout=0.2) as ser:
        while time.monotonic() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            logging.info("K210: %s", line)
            if any(m in line for m in stop_markers):
                logging.info("serial monitor: stop marker found")
                return
    logging.info("serial monitor: timeout")


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Build and send ESP8285 firmware to K210 WiFi-SPI bridge")
    ap.add_argument("--host", help="ESP bridge IP, visible in log as 'kesp: ip=...' or on router DHCP list")
    ap.add_argument("--port", type=int, default=DEFAULT_TCP_PORT, help="TCP bridge port, default 7777")
    ap.add_argument("--env", default="esp8285", help="PlatformIO environment, default esp8285")
    ap.add_argument("--build-dir", help="Override PlatformIO build directory")
    ap.add_argument("--no-build", action="store_true", help="Do not run PlatformIO build, only use existing binaries")
    ap.add_argument("--firmware", help="Send one explicit .bin instead of PlatformIO split outputs")
    ap.add_argument("--remote-name", help="Remote file name for --firmware, default esp8285_at.bin")
    ap.add_argument("--offset", default="0x0", help="Flash offset for --firmware, default 0x0")
    ap.add_argument("--dry-run", action="store_true", help="Build/prepare only, do not send over TCP")
    ap.add_argument("--timeout", type=float, default=30.0, help="TCP timeout seconds")
    ap.add_argument("--monitor", help="After upload, wait for Enter and monitor K210 serial COM port, e.g. COM7")
    ap.add_argument("--monitor-baud", type=int, default=115200)
    ap.add_argument("--monitor-timeout", type=int, default=180)
    return ap.parse_args()


def main() -> int:
    log_path = setup_logging()
    args = parse_args()
    logging.info("repo: %s", ROOT)
    logging.info("log: %s", log_path)

    if not args.no_build and not args.firmware:
        build_platformio(args.env)
    else:
        logging.info("build skipped")

    parts = collect_parts(args)
    payload_files = write_payload(parts)

    if args.dry_run or not args.host:
        logging.info("payload prepared only: %s", OUT_DIR)
        if not args.host:
            logging.info("No --host given. Use --host <ESP_IP> to send through the bridge.")
        return 0

    upload_payload(args.host, args.port, payload_files, args.timeout)

    if args.monitor:
        input("Insert/reboot K210 now, then press Enter here to start serial log capture...")
        monitor_serial(args.monitor, args.monitor_baud, args.monitor_timeout)

    logging.info("done. Send this log if flashing fails: %s", log_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
