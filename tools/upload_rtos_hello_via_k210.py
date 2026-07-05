#!/usr/bin/env python3
from __future__ import annotations
import argparse, json, logging, sys, time
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import send_flash_payload as base
from send_flash_payload_auto import KsdAutoClient, KSD_DEFAULT_BAUD

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = ROOT / 'esp8266_rtos_clean' / 'hello_uart' / 'build'


def parts_from_build(build_dir: Path):
    boot = build_dir / 'bootloader' / 'bootloader.bin'
    part = build_dir / 'partitions_1mb_singleapp.bin'
    app = build_dir / 'esp8285-hello-uart.bin'
    missing = [p for p in (boot, part, app) if not p.exists()]
    if missing:
        raise SystemExit('Missing RTOS hello build artifacts: ' + ', '.join(str(p) for p in missing))
    return base.split_large_parts([
        base.FlashPart(boot, 'esp_boot.bin', 0x00000000),
        base.FlashPart(part, 'esp_part.bin', 0x00008000),
        base.FlashPart(app, 'esp_app.bin', 0x00010000),
    ])


def wait_result(c: KsdAutoClient):
    c.command_prompt()
    logging.info('sd-uart FLASH_ESP')
    c.write_command('FLASH_ESP\n')
    c.stage_line(('KSD:FLASHING', 'KSD:ERR'), 'FLASH_ESP start ACK')

    success_markers = (
        'BOOT: ESP8285 / ESP8266 RTOS SDK hello_uart',
        'kesp: spi slave ready',
        'alive seq=',
    )
    hard_fail_markers = (
        'ESP flash result: FAIL',
        'KSD:FLASH_FAIL',
        '[esp-flash] connect failed',
        '[esp-flash] image missing',
        '[esp-flash] bad image size',
        '[esp-flash] write failed',
        '[esp-flash] finish failed',
        'csum err',
        'checksum',
        'invalid header',
        'wrong magic',
        'Fatal exception',
        'Exception (',
    )

    deadline = time.monotonic() + 60.0
    saw_flash_ok = False
    while time.monotonic() < deadline:
        line = c.read_line_once()
        if not line:
            continue
        if 'KSD:FLASH_OK' in line or 'ESP flash result: OK' in line or '[esp-flash] all parts done' in line:
            saw_flash_ok = True
        if any(marker in line for marker in success_markers):
            logging.info('ESP8266_RTOS_SDK hello/SPI marker found')
            return
        if any(marker in line for marker in hard_fail_markers):
            raise SystemExit('ESP boot/flash failed: ' + line)

    if saw_flash_ok:
        raise SystemExit('Timeout after KSD:FLASH_OK: ESP did not print RTOS hello/SPI-ready log')
    raise SystemExit('Timeout waiting ESP8266_RTOS_SDK hello UART log')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sd-uart', required=True)
    ap.add_argument('--sd-baud', type=int, default=KSD_DEFAULT_BAUD)
    ap.add_argument('--build-dir', default=str(BUILD_DIR))
    ap.add_argument('--connect-timeout', type=float, default=25.0)
    args = ap.parse_args()

    log_path = base.setup_logging()
    logging.info('repo: %s', ROOT)
    logging.info('log: %s', log_path)
    logging.info('mode: MSYS ESP8266_RTOS_SDK hello via K210 KSD')

    parts = parts_from_build(Path(args.build_dir))
    c = KsdAutoClient(args.sd_uart, args.sd_baud, 'none', args.connect_timeout)
    try:
        c.connect()
        # Deterministic RTOS hello bring-up must not pre-read flash.json.
        # The old GET flash.json was a hidden SD-init entry point before upload.
        logging.info('sd-uart: skip pre-upload GET flash.json; writing fresh RTOS hello one-shot config')
        cfg = base.patch_flash_config(None, parts)
        files = base.write_payload(parts, cfg)
        ordered = [p for p in files if p.name != 'flash.json'] + [p for p in files if p.name == 'flash.json']
        logging.info('new flash_once section:')
        logging.info(json.dumps(cfg.get('flash_once', {}), indent=2))
        for p in ordered:
            logging.info('PUT %s (%d bytes)', p.name, p.stat().st_size)
            c.put_file(p, p.name)
        wait_result(c)
    finally:
        c.close()
    logging.info('done')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
