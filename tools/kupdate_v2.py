#!/usr/bin/env python3
"""Stream one K210 slot image through ESP/KLINK and stop on first fault."""
import argparse
import hashlib
import socket
import struct
import sys
import time
import zlib
from pathlib import Path

MAGIC = 0x3250554B
VERSION = 2
HEADER = struct.Struct("<IHHI32sI")
STATUS = struct.Struct("<IBBBBIIII")
APP_HEADER = struct.Struct("<8I")
STATE = {1: "READY", 2: "RECEIVING", 3: "VERIFYING", 4: "VERIFIED", 5: "COMMITTED", 6: "BOOTING", 255: "FAILED"}


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise RuntimeError(f"connection closed at {len(data)}/{size} response bytes")
        data.extend(part)
    return bytes(data)


def recv_status(sock: socket.socket) -> tuple[int, int, int, int, int, int]:
    raw = recv_exact(sock, STATUS.size)
    magic, state, error, slot, _reserved, offset, image_size, detail, crc = STATUS.unpack(raw)
    if magic != MAGIC or zlib.crc32(raw[:-4]) & 0xFFFFFFFF != crc:
        raise RuntimeError(f"invalid K210 status magic/CRC raw={raw.hex()}")
    name = STATE.get(state, f"STATE-{state}")
    print(f"K210 {name} slot={'AB'[slot] if slot < 2 else '?'} offset={offset}/{image_size} detail={detail}")
    if state == 255 or error:
        raise RuntimeError(f"K210 stopped update: error={error} detail={detail} offset={offset}")
    return state, error, slot, offset, image_size, detail


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--host", required=True, help="ESP STA IPv4 address from STA_READY log")
    parser.add_argument("--port", type=int, default=21002)
    args = parser.parse_args()
    image = args.image.read_bytes()
    if not 0 < len(image) <= 5 * 1024 * 1024:
        raise RuntimeError(f"image size outside slot: {len(image)}")
    magic, magic_inv, load, entry, declared, *_ = APP_HEADER.unpack_from(image)
    if (magic, magic_inv) != (0x4B323130, 0xB4CDCEDF):
        raise RuntimeError("not a K210 v2 slot image: header magic mismatch")
    if load != 0x80100000 or not 0x80100000 <= entry < 0x80600000 or declared != len(image):
        raise RuntimeError(f"slot header mismatch load=0x{load:08x} entry=0x{entry:08x} declared={declared} actual={len(image)}")

    digest = hashlib.sha256(image).digest()
    prefix = HEADER.pack(MAGIC, VERSION, HEADER.size, len(image), digest, 0)
    header = prefix[:-4] + struct.pack("<I", zlib.crc32(prefix[:-4]) & 0xFFFFFFFF)
    print(f"CONNECT {args.host}:{args.port} bytes={len(image)} sha256={digest.hex()}")
    started = time.monotonic()
    with socket.create_connection((args.host, args.port)) as sock:
        sock.sendall(header)
        state, *_ = recv_status(sock)
        if state != 1:
            raise RuntimeError(f"expected READY, got {STATE.get(state, state)}")
        sent = 0
        view = memoryview(image)
        while sent < len(image):
            end = min(sent + 64 * 1024, len(image))
            sock.sendall(view[sent:end])
            sent = end
            print(f"\rSEND {sent}/{len(image)}", end="", flush=True)
        print()
        while True:
            state, *_ = recv_status(sock)
            if state == 5:
                break
    elapsed = time.monotonic() - started
    print(f"KUPDATE_V2_PASS bytes={len(image)} seconds={elapsed:.3f} Bps={len(image)/elapsed:.0f} sha256={digest.hex()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"KUPDATE_V2_FAIL {exc}", file=sys.stderr)
        raise SystemExit(1)
