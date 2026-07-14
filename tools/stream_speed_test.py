#!/usr/bin/env python3
import argparse
import re
import socket
import struct
import sys
import threading
import time


def pattern(offset):
    return (0x6D ^ offset ^ (offset >> 9) ^ (offset >> 21)) & 0xFF


class Console:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = bytearray()

    def wait_for(self, expected, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            newline = self.buffer.find(b"\n")
            if newline >= 0:
                line = self.buffer[:newline + 1]
                del self.buffer[:newline + 1]
                text = line.decode("ascii", "replace").strip()
                if expected in text:
                    return text
                continue
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            self.buffer.extend(data)
        raise RuntimeError(f"console response timeout: {expected}")

    def command(self, command, expected):
        self.sock.sendall((command + "\n").encode())
        return self.wait_for(expected)


def main():
    parser = argparse.ArgumentParser(description="K210 WiFi stream speed test")
    parser.add_argument("host", nargs="?", default="192.168.0.105")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--downlink", type=int, default=500000,
                        help="downlink rate in bit/s")
    args = parser.parse_args()

    uplink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    uplink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    uplink.bind(("", 0))
    uplink.settimeout(0.1)
    uplink.sendto(b"speed", (args.host, 21011))

    console_sock = socket.create_connection((args.host, 21012), 5)
    console_sock.settimeout(0.2)
    console = Console(console_sock)
    console.wait_for("SPI stage=")
    baseline_status = console.command("status", "status down=")
    baseline_rx = int(re.search(r"\brx=(\d+)", baseline_status).group(1))
    print(console.command("bench on", "uplink benchmark enabled"))
    print(console.command("bench down reset", "downlink benchmark enabled"))

    packets = []

    def receive_uplink():
        deadline = time.perf_counter() + args.seconds
        while time.perf_counter() < deadline:
            try:
                packets.append(uplink.recvfrom(1400)[0])
            except socket.timeout:
                pass

    receiver = threading.Thread(target=receive_uplink)
    receiver.start()
    downlink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    started = time.perf_counter()
    sent = 0
    while time.perf_counter() - started < args.seconds:
        payload = bytes(pattern(sent + i) for i in range(1200))
        downlink.sendto(payload, (args.host, 21010))
        sent += len(payload)
        delay = started + sent * 8 / args.downlink - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
    receiver.join()
    elapsed = time.perf_counter() - started

    drain_deadline = time.monotonic() + 5.0
    delivered = 0
    while delivered < sent and time.monotonic() < drain_deadline:
        status = console.command("status", "status down=")
        delivered = int(re.search(r"\brx=(\d+)", status).group(1)) - baseline_rx

    down_status = console.command("bench down off",
                                  "downlink benchmark disabled")
    print(console.command("bench off", "uplink benchmark disabled"))
    console_sock.close()
    downlink.close()
    uplink.close()

    received = 0
    corrupt = 0
    gaps = 0
    first_offset = None
    last_offset = None
    for packet in packets:
        if len(packet) < 8:
            corrupt += 1
            continue
        magic, offset = struct.unpack_from("<II", packet)
        payload = packet[8:]
        if last_offset is not None and offset != last_offset:
            gaps += 1
        if first_offset is None:
            first_offset = offset
        last_offset = offset + len(payload)
        received += len(payload)
        if magic != 0x3250554B or payload != bytes(
                pattern(offset + i) for i in range(len(payload))):
            corrupt += 1

    source = 0 if first_offset is None else last_offset - first_offset
    match = re.search(r"bytes=(\d+) errors=(\d+)", down_status)
    if not match:
        raise RuntimeError(f"invalid downlink result: {down_status}")
    down_received, down_corrupt = map(int, match.groups())
    down_loss = max(0, sent - down_received)

    print(down_status)
    print(f"uplink: source {source * 8 / args.seconds / 1e6:.3f} Mb/s, "
          f"received {received * 8 / args.seconds / 1e6:.3f} Mb/s, "
          f"gaps {gaps}, corrupt {corrupt}")
    print(f"downlink: sent {sent * 8 / elapsed / 1e6:.3f} Mb/s, "
          f"delivered {down_received * 8 / elapsed / 1e6:.3f} Mb/s, "
          f"loss {down_loss * 100 / sent:.2f}%, corrupt {down_corrupt}")

    passed = received > 0 and corrupt == 0 and down_loss == 0 and down_corrupt == 0
    print(f"RESULT: {'PASS' if passed else 'FAIL'}")
    if not passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
