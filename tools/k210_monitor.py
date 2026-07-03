#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser(description="K210 UART monitor that does not toggle DTR/RTS")
    ap.add_argument("port", help="COM port, e.g. COM12")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial is not installed. Run: py -3 -m pip install pyserial", file=sys.stderr)
        return 1

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.bytesize = 8
    ser.parity = "N"
    ser.stopbits = 1
    ser.timeout = 0.05
    ser.write_timeout = 0.05
    ser.xonxoff = False
    ser.rtscts = False
    ser.dsrdtr = False

    # Important for this board: CH340/auto-boot wiring uses DTR/RTS for RESET/BOOT.
    # Set the inactive states before open so the monitor does not reset/hold K210.
    ser.dtr = False
    ser.rts = False

    print(f"K210 monitor {args.port} @ {args.baud}, DTR=0 RTS=0")
    print("This monitor is read-only and does not reset K210. Press Ctrl+C to exit.")
    ser.open()
    try:
        ser.dtr = False
        ser.rts = False
        while True:
            data = ser.read(4096)
            if data:
                text = data.decode("utf-8", errors="replace")
                print(text, end="", flush=True)
            else:
                time.sleep(0.01)
    except KeyboardInterrupt:
        print("\nmonitor stopped")
    finally:
        try:
            ser.dtr = False
            ser.rts = False
        except Exception:
            pass
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
