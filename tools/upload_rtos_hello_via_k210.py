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
    deadline = time.monotonic() + 240.0
    while time.monotonic() < deadline:
        line = c.read_line_once()
        if not line:
            continue
        if 'BOOT: ESP8285 / ESP8266 RTOS SDK hello_uart' in line or 'alive seq=' in line:
            logging.info('ESP8266_RTOS_SDK hello marker found')
            return
        if 'ESP flash result: FAIL' in line or 'KSD:FLASH_FAIL' in line or '[esp-flash] connect failed' in line:
            raise SystemExit('K210 ESP flash failed: ' + line)
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
        existing = c.get_file('flash.json')
        cfg = base.patch_flash_config(existing, parts)
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
