#!/usr/bin/env python3
import argparse
import socket
import struct
import time
import zlib
from pathlib import Path


MAGIC = 0x41544F45
FLAG_DUAL = 1
PORT = 21001


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("ESP closed OTA connection")
        data.extend(chunk)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description="Upload an ESP8285 OTA image")
    parser.add_argument("host")
    parser.add_argument("image", type=Path)
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()

    image = args.image.read_bytes()
    if (len(image) == 0 or len(image) % 2 or image[0] != 0xE9 or \
            image[len(image) // 2] != 0xE9):
        raise RuntimeError("expected the dual-slot build/*.ota.bin image")
    slot_size = len(image) // 2
    header = struct.pack("<IIII", MAGIC, slot_size,
                         zlib.crc32(image) & 0xFFFFFFFF, FLAG_DUAL)

    started = time.perf_counter()
    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        sock.settimeout(None)
        sock.sendall(header)
        magic, result, received, detail = struct.unpack(
            "<IIII", recv_exact(sock, 16))
        if magic != MAGIC or result != 0xFFFFFFFF:
            raise RuntimeError(
                f"OTA prepare failed result={result} detail=0x{detail:08x}")
        prepared = time.perf_counter()
        slot_index = detail - 0x10
        if slot_index not in (0, 1):
            raise RuntimeError(f"invalid ESP OTA slot subtype {detail}")
        payload = image[slot_index * slot_size:(slot_index + 1) * slot_size]
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        print(f"ESP_OTA_READY erase={prepared - started:.3f}s slot={slot_index}",
              flush=True)
        sock.settimeout(30)
        sock.sendall(struct.pack("<I", crc))
        sent = 0
        reported = 0
        view = memoryview(payload)
        while sent < len(payload):
            end = min(sent + 4096, len(payload))
            sock.sendall(view[sent:end])
            magic, result, received, detail = struct.unpack(
                "<IIII", recv_exact(sock, 16))
            if magic != MAGIC or result != 0xFFFFFFFE or received != end:
                raise RuntimeError(
                    f"OTA write failed result={result} received={received} "
                    f"detail=0x{detail:08x}")
            sent = end
            if sent - reported >= 131072 or sent == len(payload):
                reported = sent
                print(f"ESP_OTA_SEND {sent}/{len(payload)}", flush=True)
        magic, result, received, detail = struct.unpack(
            "<IIII", recv_exact(sock, 16))

    if magic != MAGIC or result != 0 or received != len(payload):
        raise RuntimeError(
            f"OTA failed result={result} received={received} detail=0x{detail:08x}")
    elapsed = time.perf_counter() - started
    transfer = time.perf_counter() - prepared
    print(
        f"ESP_OTA_OK bytes={received} crc=0x{crc:08x} "
        f"rate={received * 8 / transfer / 1e6:.3f} Mb/s "
        f"total={elapsed:.3f}s")


if __name__ == "__main__":
    main()
