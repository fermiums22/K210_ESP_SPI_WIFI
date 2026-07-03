#!/usr/bin/env python3
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
from send_flash_payload_auto import KsdAutoClient, KSD_DEFAULT_BAUD  # noqa: E402


WRONG_FW_MARKERS = (
    "[pin-test-main]",
    "kesp-spi-test: no WiFi, no TCP, no SD; ESP8266 Arduino SPISlave only",
    "[pure-spi] scan start",
)

EXPECTED_FW_MARKERS = (
    "[spi-loader-main] start safe one-shot ESP loader + pure SPI scanner",
    "[spi-loader-main] KSD loader window",
)


class RtosSpiKsdClient(KsdAutoClient):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.seen_loader_marker = False

    def read_line_once(self, *args, **kwargs) -> str | None:
        line = super().read_line_once(*args, **kwargs)
        if not line:
            return line
        if any(m in line for m in EXPECTED_FW_MARKERS):
            self.seen_loader_marker = True
        if any(m in line for m in WRONG_FW_MARKERS):
            raise SystemExit(
                "Wrong K210 firmware is running: saw old pin-test/Arduino SPI scanner logs. "
                "The K210 diagnostic-loader was not flashed successfully. Re-run after a clean K210 flash."
            )
        return line

    def connect(self) -> None:
        try:
            super().connect()
        except TimeoutError as exc:
            raise SystemExit(
                "KSD handshake timeout. Most likely the K210 diagnostic-loader is not running. "
                "Check the K210 flash step above; it must print the [spi-loader-main] marker."
            ) from exc


def wait_flash_result(client: KsdAutoClient) -> None:
    client.command_prompt()
    logging.info("sd-uart FLASH_ESP")
    client.write_command("FLASH_ESP\n")
    client.stage_line(("KSD:FLASHING", "KSD:ERR"), "FLASH_ESP start ACK")

    deadline = time.monotonic() + 240.0
    while time.monotonic() < deadline:
        line = client.read_line_once()
        if not line:
            continue
        if line.startswith("KSD:FLASH_OK"):
            logging.info("sd-uart ESP flash OK")
            return
        if line.startswith("KSD:FLASH_FAIL") or "ESP flash result: FAIL" in line:
            raise SystemExit(f"K210 ESP flash failed: {line}")
    raise SystemExit("K210 ESP flash timeout")


def start_spi_scan(client: KsdAutoClient) -> None:
    client.command_prompt()
    logging.info("sd-uart RUN_SPI")
    client.write_command("RUN_SPI\n")
    client.stage_line(("KSD:RUNSPI", "KSD:DONE", "KSD:ERR"), "RUN_SPI ACK")


def monitor_spi_verdict(client: KsdAutoClient) -> None:
    logging.info("stage: monitor pure SPI verdict")
    deadline = time.monotonic() + 180.0
    saw_esp_ready = False
    while time.monotonic() < deadline:
        line = client.read_line_once()
        if not line:
            continue
        if "kesp: spi slave ready" in line or "kesp-rtos-spi-test: ready" in line:
            saw_esp_ready = True
        if "[pure-spi] VERDICT SPI_OK" in line:
            logging.info("pure SPI verdict OK")
            return
        if "[pure-spi] VERDICT SPI_FAIL" in line:
            raise SystemExit(f"pure SPI verdict FAIL: {line}")
    if saw_esp_ready:
        raise SystemExit("pure SPI verdict timeout: ESP ready was seen, but K210 printed no verdict")
    raise SystemExit("pure SPI verdict timeout: ESP ready was not seen")


def upload_and_run(args: argparse.Namespace, parts: list[base.FlashPart]) -> None:
    client = RtosSpiKsdClient(args.sd_uart, args.sd_baud, args.auto_reset, args.connect_timeout)
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

        wait_flash_result(client)
        start_spi_scan(client)
        monitor_spi_verdict(client)
    finally:
        client.close()


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Upload ESP8285 RTOS SPI test through K210 diagnostic-loader and monitor verdict")
    ap.add_argument("--sd-uart", required=True)
    ap.add_argument("--sd-baud", type=int, default=KSD_DEFAULT_BAUD)
    ap.add_argument("--auto-reset", default="none", choices=("dan", "rts", "dtr", "both", "none"))
    ap.add_argument("--connect-timeout", type=float, default=25.0)
    ap.add_argument("--env", default="esp8285")
    ap.add_argument("--build-dir")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--firmware")
    ap.add_argument("--remote-name")
    ap.add_argument("--offset", default="0x0")
    ap.add_argument("--full-flash", action="store_true")
    return ap.parse_args()


def main() -> int:
    log_path = base.setup_logging()
    args = parse_args()
    logging.info("repo: %s", base.ROOT)
    logging.info("log: %s", log_path)
    logging.info("mode: RTOS SPI automatic loader")

    if not args.no_build and not args.firmware:
        base.build_platformio(args.env)
    else:
        logging.info("build skipped")

    parts = base.collect_parts(args)
    upload_and_run(args, parts)
    logging.info("done. Log: %s", log_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
