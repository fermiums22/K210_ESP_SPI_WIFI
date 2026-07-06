#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import logging
import shutil
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / "esp8266_rtos_clean" / "hello_uart" / "build"
OUT_DIR = ROOT / "out" / "flash_payload"
LOG_DIR = ROOT / "logs"
KSD_MAGIC = b"KSD1\n"
KSD_CHUNK = 4096
WIFI_CONFIG_OFFSET = 0x000E0000
SAFE_PART_BYTES = 0x40000


@dataclass(frozen=True)
class FlashPart:
    source: Path
    remote_name: str
    offset: int
    source_offset: int = 0
    size: int | None = None


def setup_logging() -> Path:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"rtos_upload_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log"
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s: %(message)s",
        datefmt="%H:%M:%S",
        handlers=[logging.StreamHandler(sys.stdout), logging.FileHandler(log_path, encoding="utf-8")],
    )
    return log_path


def part_size(p: FlashPart) -> int:
    return p.size if p.size is not None else p.source.stat().st_size - p.source_offset


def split_part(p: FlashPart) -> list[FlashPart]:
    total = part_size(p)
    if total <= SAFE_PART_BYTES:
        return [p]
    out: list[FlashPart] = []
    pos = 0
    while pos < total:
        n = min(SAFE_PART_BYTES, total - pos)
        out.append(FlashPart(p.source, f"esp_{p.offset + pos:06x}.bin", p.offset + pos, p.source_offset + pos, n))
        pos += n
    logging.info("split %s size=%d -> %d parts", p.source, total, len(out))
    return out


def write_wifi_config() -> Path:
    path = ROOT / "out" / "wifi_config_src.bin"
    path.parent.mkdir(parents=True, exist_ok=True)
    body = {
        "mode": "sta_ap",
        "ssid": "ELECTRONICS",
        "pass": "bdc123print",
        "tcp_port": 18080,
    }
    data = ("KESPJSON\n" + json.dumps(body, indent=2) + "\n").encode("utf-8")
    if len(data) > 4096:
        raise SystemExit("wifi config is larger than 4096 bytes")
    path.write_bytes(data + b"\xff" * (4096 - len(data)))
    return path


def build_parts(build_dir: Path) -> list[FlashPart]:
    boot = build_dir / "bootloader" / "bootloader.bin"
    table = build_dir / "partitions_1mb_singleapp.bin"
    app = build_dir / "esp8285-hello-uart.bin"
    missing = [p for p in (boot, table, app) if not p.exists()]
    if missing:
        raise SystemExit("missing ESP build artifacts: " + ", ".join(str(p) for p in missing))
    raw = [
        FlashPart(boot, "esp_boot.bin", 0x00000000),
        FlashPart(table, "esp_part.bin", 0x00008000),
        FlashPart(app, "esp_app.bin", 0x00010000),
        FlashPart(write_wifi_config(), "wifi_config.json", WIFI_CONFIG_OFFSET),
    ]
    parts: list[FlashPart] = []
    for p in raw:
        parts.extend(split_part(p))
    return parts


def copy_part(p: FlashPart, dst: Path) -> None:
    if p.source_offset == 0 and p.size is None:
        shutil.copyfile(p.source, dst)
        return
    left = part_size(p)
    with p.source.open("rb") as src, dst.open("wb") as out:
        src.seek(p.source_offset)
        while left:
            chunk = src.read(min(65536, left))
            if not chunk:
                raise SystemExit(f"unexpected EOF slicing {p.source}")
            out.write(chunk)
            left -= len(chunk)


def write_payload(parts: list[FlashPart]) -> list[Path]:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    OUT_DIR.mkdir(parents=True)
    written: list[Path] = []
    for p in parts:
        dst = OUT_DIR / p.remote_name
        copy_part(p, dst)
        logging.info("payload %s -> %s offset=0x%08x size=%d", p.source.name, dst.name, p.offset, dst.stat().st_size)
        written.append(dst)
    cfg = {
        "flash_once": {
            "enabled": 1,
            "source": "K210_ESP_SPI_WIFI/tools/upload_rtos_hello_via_k210.py",
            "requested_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat(),
            "esp": {
                "enabled": 1,
                "parts": [{"file": p.remote_name, "offset": f"0x{p.offset:08x}"} for p in parts],
            },
        }
    }
    cfg_path = OUT_DIR / "flash.json"
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    written.append(cfg_path)
    logging.info("payload flash.json generated")
    return written


class StrictKsd:
    def __init__(self, port: str, baud: int):
        try:
            import serial  # type: ignore
        except ImportError as exc:
            raise SystemExit("pyserial is missing; install requirements.txt") from exc
        # Keep a finite timeout only for the initial boot-banner drain. After the
        # KSD session is established, reads are blocking so long K210 operations
        # such as ESP flashing are not misreported as protocol failures.
        self.ser = serial.Serial(port, baud, timeout=1.0, write_timeout=5.0)
        self.ser.dtr = False
        self.ser.rts = False
        self.have_prompt = False
        self.quiet_ack_count = 0

    def close(self) -> None:
        self.ser.close()

    def line(self, stage: str) -> str:
        raw = self.ser.readline()
        if not raw:
            raise SystemExit(f"{stage}: no line from K210")
        line = raw.decode("utf-8", errors="replace").rstrip()
        if line == "KSD:B":
            self.quiet_ack_count += 1
        else:
            logging.info("K210: %s", line)
        return line

    def expect(self, prefixes: tuple[str, ...], stage: str) -> str:
        while True:
            line = self.line(stage)
            if line.startswith("KSD:ERR") or line.startswith("KSD:FLASH_FAIL"):
                raise SystemExit(f"{stage}: {line}")
            if line.startswith(prefixes):
                return line

    def connect(self) -> None:
        logging.info("KSD strict connect: drain boot banner, send one magic after listener/quiet")
        while True:
            raw = self.ser.readline()
            if not raw:
                break
            line = raw.decode("utf-8", errors="replace").rstrip()
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
        line = self.expect(("KSD:HELLO", "KSD:CMD"), "connect")
        if line.startswith("KSD:CMD"):
            self.have_prompt = True
        self.ser.timeout = None

    def prompt(self) -> None:
        if self.have_prompt:
            self.have_prompt = False
            return
        self.expect(("KSD:CMD",), "command prompt")

    def put_file(self, path: Path, remote: str) -> None:
        size = path.stat().st_size
        self.prompt()
        logging.info("PUT %s size=%d", remote, size)
        self.ser.write(f"PUT {remote} {size}\n".encode("ascii"))
        self.ser.flush()
        line = self.expect(("KSD:GO",), f"PUT {remote} GO")
        chunk_size = KSD_CHUNK
        parts = line.split()
        if len(parts) >= 2:
            chunk_size = int(parts[1])
        self.expect(("KSD:READYDATA",), f"PUT {remote} READYDATA")
        sent = 0
        ack_start = self.quiet_ack_count
        with path.open("rb") as f:
            while sent < size:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                self.ser.write(chunk)
                self.ser.flush()
                sent += len(chunk)
                self.expect(("KSD:B",), f"PUT {remote} block {sent}/{size}")
                if sent == size or (sent % (32 * 1024)) == 0:
                    logging.info("PUT %s progress %d/%d bytes", remote, sent, size)
        self.expect(("KSD:OK",), f"PUT {remote} final")
        logging.info("PUT %s OK bytes=%d chunk=%d acks=%d", remote, sent, chunk_size, self.quiet_ack_count - ack_start)

    def flash_esp(self) -> None:
        self.prompt()
        logging.info("FLASH_ESP")
        self.ser.write(b"FLASH_ESP\n")
        self.ser.flush()
        self.expect(("KSD:FLASHING",), "FLASH_ESP start")
        while True:
            line = self.line("FLASH_ESP monitor")
            if line.startswith("KSD:FLASH_OK"):
                return
            if "kesp: tcp server ready" in line or "kesp: wifi connected" in line or "kesp: fallback AP" in line:
                return
            if "FAIL" in line or "Exception" in line or "invalid header" in line or "checksum" in line:
                raise SystemExit(f"FLASH_ESP monitor failed: {line}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sd-uart", required=True)
    ap.add_argument("--sd-baud", type=int, default=921600)
    ap.add_argument("--build-dir", default=str(BUILD_DIR))
    args = ap.parse_args()

    log_path = setup_logging()
    logging.info("repo: %s", ROOT)
    logging.info("log: %s", log_path)
    parts = build_parts(Path(args.build_dir))
    files = write_payload(parts)
    ordered = [p for p in files if p.name != "flash.json"] + [p for p in files if p.name == "flash.json"]

    ksd = StrictKsd(args.sd_uart, args.sd_baud)
    try:
        ksd.connect()
        for path in ordered:
            ksd.put_file(path, path.name)
        ksd.flash_esp()
    finally:
        ksd.close()
    logging.info("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
