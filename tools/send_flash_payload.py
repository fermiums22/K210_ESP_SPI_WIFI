#!/usr/bin/env python3
"""
Build and send ESP8285 flash payload through K210.

Preferred flow:
  PC script -> K210 debug UART KSD1 protocol -> SD card files + flash.json
  PC script -> K210 FLASH_ESP command
  K210 -> ESP serial flasher -> ESP normal boot/logs
"""
from __future__ import annotations

import argparse
import json
import logging
import os
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
KSD_CHUNK = 4096

# Keep normal ESP images whole.  Splitting an ESP8266 app image into separately
# flashed ranges made the bootloader hit `csum err` even though each individual
# flasher transaction reported OK.  Only very large/full images are split, and
# then on a large sector-aligned boundary.
KSD_SAFE_FLASH_PART_BYTES = 0x40000  # 256 KiB, flash-sector aligned
K210_MAX_ESP_FLASH_PARTS = 8


@dataclass(frozen=True)
class FlashPart:
    source: Path
    remote_name: str
    offset: int
    source_offset: int = 0
    size: int | None = None


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
    proc = subprocess.Popen(cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
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


def part_payload_size(part: FlashPart) -> int:
    if part.size is not None:
        return part.size
    return part.source.stat().st_size - part.source_offset


def split_large_part(part: FlashPart) -> list[FlashPart]:
    total = part_payload_size(part)
    if total <= KSD_SAFE_FLASH_PART_BYTES:
        return [part]

    chunks: list[FlashPart] = []
    pos = 0
    while pos < total:
        chunk = min(KSD_SAFE_FLASH_PART_BYTES, total - pos)
        chunks.append(FlashPart(
            source=part.source,
            remote_name=f"esp_{part.offset + pos:06x}.bin",
            offset=part.offset + pos,
            source_offset=part.source_offset + pos,
            size=chunk,
        ))
        pos += chunk

    if len(chunks) > K210_MAX_ESP_FLASH_PARTS:
        raise SystemExit(
            f"Image {part.source} is {total} bytes and would need {len(chunks)} KSD-safe parts. "
            f"Current K210 ESP flash config supports only {K210_MAX_ESP_FLASH_PARTS} parts."
        )

    logging.info("payload split: %s offset=0x%08X size=%d -> %d x <= %d byte parts",
                 part.source, part.offset, total, len(chunks), KSD_SAFE_FLASH_PART_BYTES)
    return chunks


def split_large_parts(parts: Iterable[FlashPart]) -> list[FlashPart]:
    out: list[FlashPart] = []
    for part in parts:
        out.extend(split_large_part(part))
    if len(out) > K210_MAX_ESP_FLASH_PARTS:
        raise SystemExit(
            f"Payload needs {len(out)} ESP flash parts, but K210 currently supports only {K210_MAX_ESP_FLASH_PARTS}."
        )
    return out


def platformio_packages_dir() -> Path:
    env_value = os.environ.get("PLATFORMIO_PACKAGES_DIR")
    if env_value:
        return Path(env_value)
    return Path.home() / ".platformio" / "packages"


def find_sdk_blob(build_dir: Path, name: str) -> Path | None:
    candidates = [
        build_dir / name,
        ROOT / name,
        ROOT / "tools" / name,
        platformio_packages_dir() / "framework-esp8266-rtos-sdk" / "bin" / name,
        platformio_packages_dir() / "framework-esp8266-rtos-sdk" / "bin" / "at" / name,
        platformio_packages_dir() / "framework-esp8266-rtos-sdk" / "bin" / "upgrade" / name,
    ]
    for candidate in candidates:
        if candidate.exists():
            logging.info("sdk blob found: %s", candidate)
            return candidate
    return None


def generated_blank_sector(build_dir: Path) -> Path:
    path = build_dir / "blank_4k_ff.bin"
    if not path.exists() or path.stat().st_size != 4096:
        path.write_bytes(b"\xff" * 4096)
    return path


def generated_init_sector(build_dir: Path) -> Path:
    existing = find_sdk_blob(build_dir, "esp_init_data_default.bin")
    if existing:
        return existing
    path = build_dir / "esp_init_data_default_min.bin"
    if not path.exists() or path.stat().st_size != 4096:
        data = bytearray(b"\xff" * 4096)
        data[0] = 0x05
        path.write_bytes(bytes(data))
    logging.warning("SDK esp_init_data_default.bin not found; using minimal generated RF init sector: %s", path)
    return path


def collect_parts(args: argparse.Namespace) -> list[FlashPart]:
    explicit_fw = maybe_path(args.firmware)
    if explicit_fw:
        if not explicit_fw.exists():
            raise SystemExit(f"Firmware file not found: {explicit_fw}")
        return split_large_parts([FlashPart(explicit_fw, args.remote_name or "esp8285_at.bin", int(args.offset, 0))])

    build_dir = maybe_path(args.build_dir) or (ROOT / ".pio" / "build" / args.env)
    boot = build_dir / "firmware.bin"
    irom = build_dir / "firmware.bin.irom0text.bin"
    merged = build_dir / "firmware_1m_merged.bin"

    if boot.exists() and irom.exists():
        blank = find_sdk_blob(build_dir, "blank.bin") or generated_blank_sector(build_dir)
        init_data = generated_init_sector(build_dir)
        parts = [
            FlashPart(boot, "esp_boot.bin", 0x00000000),
            FlashPart(irom, "esp_irom.bin", 0x00020000),
            FlashPart(blank, "rfcal.bin", 0x000FB000),
            FlashPart(init_data, "init.bin", 0x000FC000),
            FlashPart(blank, "sysblk.bin", 0x000FE000),
        ]
        return split_large_parts(parts)

    if args.full_flash:
        if not merged.exists():
            raise SystemExit(f"--full-flash requested, but merged image not found: {merged}")
        logging.warning("Using FULL 1 MB merged image. This erases/writes the whole ESP8285 flash.")
        return split_large_parts([FlashPart(merged, "esp8285_full_1m.bin", 0x00000000)])

    if boot.exists():
        logging.info("Using PlatformIO firmware.bin at 0x00000000 (default safe mode).")
        return split_large_parts([FlashPart(boot, "esp8285_at.bin", 0x00000000)])

    if merged.exists():
        logging.warning("Only merged 1 MB image was found; using it because firmware.bin is missing.")
        return split_large_parts([FlashPart(merged, "esp8285_full_1m.bin", 0x00000000)])

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
                "parts": [{"file": p.remote_name, "offset": f"0x{p.offset:08x}"} for p in parts],
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


def copy_part_payload(part: FlashPart, dst: Path) -> None:
    if part.source_offset == 0 and part.size is None:
        shutil.copyfile(part.source, dst)
        return

    remaining = part_payload_size(part)
    with part.source.open("rb") as src, dst.open("wb") as out:
        src.seek(part.source_offset)
        while remaining:
            chunk = src.read(min(65536, remaining))
            if not chunk:
                raise SystemExit(f"Unexpected EOF while slicing {part.source}")
            out.write(chunk)
            remaining -= len(chunk)


def write_payload(parts: list[FlashPart], cfg: dict) -> list[Path]:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    written: list[Path] = []

    for part in parts:
        dst = OUT_DIR / part.remote_name
        copy_part_payload(part, dst)
        logging.info("payload: %s[%d:+%s] -> %s @ 0x%08X (%d bytes)",
                     part.source, part.source_offset, "EOF" if part.size is None else str(part.size),
                     part.remote_name, part.offset, dst.stat().st_size)
        written.append(dst)

    cfg_path = OUT_DIR / "flash.json"
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    logging.info("payload: generated %s", cfg_path)
    written.append(cfg_path)
    return written


def send_tcp(host: str, port: int, paths: list[Path]) -> None:
    with socket.create_connection((host, port), timeout=10) as s:
        for path in paths:
            data = path.read_bytes()
            name = path.name.encode("utf-8")
            header = b"PUT " + str(len(name)).encode() + b" " + str(len(data)).encode() + b"\n"
            s.sendall(header)
            s.sendall(name)
            s.sendall(data)
            logging.info("tcp: sent %s (%d bytes)", path.name, len(data))


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Build/send ESP8285 flash payload via K210")
    ap.add_argument("--env", default="esp8285")
    ap.add_argument("--build-dir")
    ap.add_argument("--firmware", help="Use explicit firmware file instead of PlatformIO output")
    ap.add_argument("--offset", default="0x00000000", help="ESP flash offset for --firmware")
    ap.add_argument("--remote-name", help="Remote SD file name for --firmware")
    ap.add_argument("--host", help="Send payload files to ESP TCP server host")
    ap.add_argument("--port", type=int, default=DEFAULT_TCP_PORT)
    ap.add_argument("--full-flash", action="store_true", help="Use merged full 1 MB image if available")
    ap.add_argument("--sd-uart", help="K210 SD UART COM port, e.g. COM12")
    ap.add_argument("--sd-baud", type=int, default=921600)
    ap.add_argument("--auto-reset", choices=("dan", "rts", "dtr", "both", "none"), default="none")
    ap.add_argument("--connect-timeout", type=float, default=20.0)
    ap.add_argument("--no-build", action="store_true", help="Do not run PlatformIO before preparing payload")
    ap.add_argument("--dry-run", action="store_true", help="Build and prepare payload only; do not upload")
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
    cfg = patch_flash_config(None, parts)
    paths = write_payload(parts, cfg)
    logging.info("payload prepared: %s", OUT_DIR)

    if args.dry_run:
        logging.info("dry-run requested: payload prepared only, no upload")
    elif args.sd_uart:
        import send_flash_payload_auto  # noqa: E402
        send_flash_payload_auto.upload_via_ksd(args, parts)
    elif args.host:
        send_tcp(args.host, args.port, paths)
    else:
        logging.info("payload prepared only: %s", OUT_DIR)
        logging.info("No --host or --sd-uart given. Use --sd-uart COMx for K210 SD upload.")
    logging.info("done. Send this log if flashing fails: %s", log_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
