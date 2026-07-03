#!/usr/bin/env python3
"""
Build and send ESP8285 flash payload through K210.

Preferred flow:
  PC script -> K210 debug UART KSD1 protocol -> SD card files + flash.json
  PC script -> K210 RESET command
  K210 boot -> sees flash.json -> disarms it -> flashes ESP8285 -> logs result

Legacy flow is still available:
  PC -> TCP PUT files to ESP8285 bridge on port 7777
  ESP8285 -> SPI frames to K210 -> SD card
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
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "logs"
OUT_DIR = ROOT / "out" / "flash_payload"
DEFAULT_TCP_PORT = 7777
KSD_MAGIC = b"KSD1\n"
KSD_CHUNK = 512


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

    if args.full_flash:
        if not merged.exists():
            raise SystemExit(f"--full-flash requested, but merged image not found: {merged}")
        logging.warning("Using FULL 1 MB merged image. This erases/writes the whole ESP8285 flash.")
        return [FlashPart(merged, "esp8285_full_1m.bin", 0x00000000)]

    if boot.exists():
        logging.info("Using PlatformIO firmware.bin at 0x00000000 (default safe mode).")
        return [FlashPart(boot, "esp8285_at.bin", 0x00000000)]

    if merged.exists():
        logging.warning("Only merged 1 MB image was found; using it because firmware.bin is missing.")
        return [FlashPart(merged, "esp8285_full_1m.bin", 0x00000000)]

    raise SystemExit(
        "No firmware image found. Run PlatformIO build first or pass --firmware path\\to\\image.bin.\n"
        f"Checked: {boot}, {irom}, {merged}"
    )


def default_flash_config(parts: Iterable[FlashPart]) -> dict:
    return {
        "flash_once": {
            "enabled": 1,
            "source": "K210_ESP_SPI_WIFI/tools/send_flash_payload.py",
            "requested_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
            "esp": {
                "enabled": 1,
                "parts": [
                    {"file": p.remote_name, "offset": f"0x{p.offset:08x}"}
                    for p in parts
                ],
            },
        }
    }


def patch_flash_config(existing_text: str | None, parts: list[FlashPart]) -> dict:
    if existing_text:
        try:
            cfg = json.loads(existing_text)
            if not isinstance(cfg, dict):
                cfg = {}
        except json.JSONDecodeError:
            logging.warning("Existing flash.json is invalid JSON; replacing flash_once section")
            cfg = {}
    else:
        cfg = {}

    wanted = default_flash_config(parts)["flash_once"]
    current = cfg.get("flash_once")
    if not isinstance(current, dict):
        current = {}
    current.update({
        "enabled": 1,
        "source": wanted["source"],
        "requested_utc": wanted["requested_utc"],
        "esp": wanted["esp"],
    })
    cfg["flash_once"] = current
    return cfg


def write_payload(parts: list[FlashPart], flash_config: dict | None = None) -> list[Path]:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    copied: list[Path] = []

    for part in parts:
        dst = OUT_DIR / part.remote_name
        shutil.copyfile(part.source, dst)
        copied.append(dst)
        logging.info("payload: %s -> %s @ 0x%08X (%d bytes)", part.source, part.remote_name, part.offset, dst.stat().st_size)

    cfg = OUT_DIR / "flash.json"
    data = flash_config if flash_config is not None else default_flash_config(parts)
    cfg.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
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


def send_one_tcp(host: str, port: int, path: Path, remote_name: str, timeout_s: float) -> None:
    size = path.stat().st_size
    logging.info("tcp send: %s -> %s:%d as %s (%d bytes)", path, host, port, remote_name, size)
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
                    logging.info("tcp progress: %s %d/%d", remote_name, sent, size)
        response = recv_line(sock, timeout_s)
    elapsed = time.monotonic() - start
    logging.info("tcp response: %s (%.1f s)", response or "<no response>", elapsed)
    if not response.startswith("OK"):
        raise SystemExit(f"Bridge did not acknowledge {remote_name}: {response!r}")


def upload_payload_tcp(host: str, port: int, files: list[Path], timeout_s: float) -> None:
    ordered = [p for p in files if p.name != "flash.json"] + [p for p in files if p.name == "flash.json"]
    for path in ordered:
        send_one_tcp(host, port, path, path.name, timeout_s)
        time.sleep(0.5)
    logging.info("tcp upload done. Reboot/power-cycle K210_AI_V7s_Plus to run flash_once.")


class KsdClient:
    def __init__(self, port: str, baud: int, timeout: float = 0.2):
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SystemExit("pyserial is not installed. Run: py -3 -m pip install -r requirements.txt") from exc
        self.serial_mod = serial
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.port = port
        self.baud = baud

    def close(self) -> None:
        self.ser.close()

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def read_ksd_line(self, timeout_s: float, prefixes: tuple[str, ...] | None = None) -> str:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            if line:
                logging.info("K210: %s", line)
            if not line.startswith("KSD:"):
                continue
            if prefixes is None or line.startswith(prefixes):
                return line
        raise TimeoutError(f"Timeout waiting for KSD line {prefixes or ''}")

    def connect(self, timeout_s: float) -> None:
        logging.info("sd-uart: opening %s @ %d", self.port, self.baud)
        logging.info("sd-uart: reset/power-cycle K210 now if it is already running normal app")
        deadline = time.monotonic() + timeout_s
        next_magic = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_magic:
                self.write(KSD_MAGIC)
                next_magic = now + 0.25
            try:
                line = self.read_ksd_line(0.25, ("KSD:HELLO", "KSD:READY"))
            except TimeoutError:
                continue
            if line.startswith("KSD:HELLO"):
                logging.info("sd-uart: connected")
                return
        raise SystemExit("K210 SD UART service did not answer. Start script, then reset K210 during its boot window.")

    def wait_cmd(self) -> None:
        self.read_ksd_line(10.0, ("KSD:CMD",))

    def get_file(self, name: str) -> str | None:
        self.wait_cmd()
        logging.info("sd-uart GET %s", name)
        self.write(f"GET {name}\n".encode("ascii"))
        line = self.read_ksd_line(10.0, ("KSD:SIZE", "KSD:MISSING", "KSD:ERR"))
        if line.startswith("KSD:MISSING"):
            logging.info("sd-uart GET %s: missing", name)
            return None
        if line.startswith("KSD:ERR"):
            raise SystemExit(f"K210 GET failed: {line}")
        size = int(line.split()[1])
        data = self.ser.read(size)
        if len(data) != size:
            raise SystemExit(f"K210 GET short read: {len(data)}/{size}")
        ok = self.read_ksd_line(10.0, ("KSD:OK", "KSD:ERR"))
        if not ok.startswith("KSD:OK"):
            raise SystemExit(f"K210 GET failed after data: {ok}")
        text = data.decode("utf-8", errors="replace")
        logging.info("sd-uart GET %s: %d bytes", name, size)
        logging.info("sd-uart existing %s: %s", name, text.replace("\n", " "))
        return text

    def put_file(self, path: Path, remote_name: str) -> None:
        size = path.stat().st_size
        self.wait_cmd()
        logging.info("sd-uart PUT %s (%d bytes)", remote_name, size)
        self.write(f"PUT {remote_name} {size}\n".encode("ascii"))
        line = self.read_ksd_line(10.0, ("KSD:GO", "KSD:ERR"))
        if not line.startswith("KSD:GO"):
            raise SystemExit(f"K210 refused PUT {remote_name}: {line}")

        sent = 0
        with path.open("rb") as f:
            while sent < size:
                chunk = f.read(KSD_CHUNK)
                if not chunk:
                    break
                self.write(chunk)
                sent += len(chunk)
                line = self.read_ksd_line(10.0, ("KSD:B", "KSD:ERR"))
                if line.startswith("KSD:ERR"):
                    raise SystemExit(f"K210 PUT failed at {sent}/{size}: {line}")
                if sent == size or sent % 32768 == 0:
                    logging.info("sd-uart progress: %s %d/%d", remote_name, sent, size)

        ok = self.read_ksd_line(20.0, ("KSD:OK", "KSD:ERR"))
        if not ok.startswith("KSD:OK"):
            raise SystemExit(f"K210 PUT failed after data: {ok}")
        logging.info("sd-uart PUT done: %s", remote_name)

    def reset_and_monitor(self, timeout_s: int) -> None:
        self.wait_cmd()
        logging.info("sd-uart RESET")
        self.write(b"RESET\n")
        self.read_ksd_line(10.0, ("KSD:RESETTING", "KSD:ERR"))
        logging.info("K210 reset requested, monitoring flash log")
        stop_markers = ("ESP flash result: OK", "ESP flash result: FAIL", "[esp-flash] done", "[esp-flash] connect failed")
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            if line:
                logging.info("K210: %s", line)
            if any(m in line for m in stop_markers):
                logging.info("sd-uart monitor: stop marker found")
                return
        logging.info("sd-uart monitor: timeout")


def upload_payload_sd_uart(port: str, baud: int, parts: list[FlashPart], timeout_s: float, monitor_timeout: int) -> None:
    client = KsdClient(port, baud)
    try:
        client.connect(timeout_s)
        existing = client.get_file("flash.json")
        cfg = patch_flash_config(existing, parts)
        files = write_payload(parts, cfg)
        ordered = [p for p in files if p.name != "flash.json"] + [p for p in files if p.name == "flash.json"]
        for path in ordered:
            client.put_file(path, path.name)
        client.reset_and_monitor(monitor_timeout)
    finally:
        client.close()


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Build and send ESP8285 firmware to K210")
    ap.add_argument("--host", help="Legacy ESP bridge IP, visible as 'kesp: ip=...'")
    ap.add_argument("--port", type=int, default=DEFAULT_TCP_PORT, help="Legacy TCP bridge port, default 7777")
    ap.add_argument("--sd-uart", help="Preferred mode: K210 debug UART COM port, e.g. COM7")
    ap.add_argument("--sd-baud", type=int, default=115200, help="K210 debug UART baud, default 115200")
    ap.add_argument("--env", default="esp8285", help="PlatformIO environment, default esp8285")
    ap.add_argument("--build-dir", help="Override PlatformIO build directory")
    ap.add_argument("--no-build", action="store_true", help="Do not run PlatformIO build, only use existing binaries")
    ap.add_argument("--firmware", help="Send one explicit .bin instead of PlatformIO output")
    ap.add_argument("--remote-name", help="Remote file name for --firmware, default esp8285_at.bin")
    ap.add_argument("--offset", default="0x0", help="Flash offset for --firmware, default 0x0")
    ap.add_argument("--full-flash", action="store_true", help="Use generated 1 MB merged image instead of default firmware.bin")
    ap.add_argument("--dry-run", action="store_true", help="Build/prepare only, do not send")
    ap.add_argument("--timeout", type=float, default=45.0, help="Connect/transfer timeout seconds")
    ap.add_argument("--monitor", help="Legacy: after TCP upload, monitor K210 serial COM port")
    ap.add_argument("--monitor-baud", type=int, default=115200)
    ap.add_argument("--monitor-timeout", type=int, default=180)
    return ap.parse_args()


def monitor_serial(port: str, baud: int, timeout_s: int) -> None:
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise SystemExit("pyserial is not installed. Run: py -3 -m pip install -r requirements.txt") from exc

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

    if args.sd_uart:
        upload_payload_sd_uart(args.sd_uart, args.sd_baud, parts, args.timeout, args.monitor_timeout)
        logging.info("done. Send this log if flashing fails: %s", log_path)
        return 0

    payload_files = write_payload(parts)

    if args.dry_run or not args.host:
        logging.info("payload prepared only: %s", OUT_DIR)
        if not args.host:
            logging.info("No --host or --sd-uart given. Use --sd-uart COMx for K210 SD upload.")
        return 0

    upload_payload_tcp(args.host, args.port, payload_files, args.timeout)

    if args.monitor:
        input("Insert/reboot K210 now, then press Enter here to start serial log capture...")
        monitor_serial(args.monitor, args.monitor_baud, args.monitor_timeout)

    logging.info("done. Send this log if flashing fails: %s", log_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
