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
            text = self.buffer.decode("ascii", "replace")
            match = re.search(expected, text)
            if match:
                line_start = text.rfind("\n", 0, match.start()) + 1
                result = text[line_start:match.end()].strip()
                del self.buffer[:match.end()]
                return result
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            self.buffer.extend(data)
        tail = self.buffer[-256:].decode("ascii", "replace")
        raise RuntimeError(f"console response timeout: {expected}; got {tail!r}")

    def command(self, command, expected):
        self.sock.sendall((command + "\n").encode())
        return self.wait_for(expected)


def main():
    parser = argparse.ArgumentParser(description="K210 WiFi stream speed test")
    parser.add_argument("host", nargs="?", default="192.168.0.105")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--uplink", type=int, default=1500000,
                        help="required uplink rate in bit/s")
    parser.add_argument("--downlink", type=int, default=500000,
                        help="downlink rate in bit/s")
    args = parser.parse_args()

    uplink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    uplink.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    uplink.bind(("", 0))
    uplink.settimeout(0.1)

    console_sock = socket.create_connection((args.host, 21012), 5)
    console_sock.settimeout(0.2)
    console = Console(console_sock)
    console.wait_for(r"SPI stage=\d+\b")
    uplink.sendto(b"speed", (args.host, 21011))
    registration, _ = uplink.recvfrom(16)
    if registration != b"KUP2":
        raise RuntimeError(f"invalid uplink registration: {registration!r}")
    status_pattern = r"status down=.*?\bderr=\d+"
    baseline_status = console.command("status", status_pattern)
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
    print(console.command("bench off", "uplink benchmark disabled"))

    drain_deadline = time.monotonic() + 5.0
    delivered = 0
    down_used = 1
    while down_used != 0 and time.monotonic() < drain_deadline:
        status = console.command("status", status_pattern)
        delivered = int(re.search(r"\brx=(\d+)", status).group(1)) - baseline_rx
        down_used = int(re.search(r"status down=(\d+)/", status).group(1))

    down_skipped = int(re.search(r"\bdloss=(\d+)", status).group(1))
    down_corrupt = int(re.search(r"\bderr=(\d+)", status).group(1))
    console_sock.sendall(b"bench down off\n")
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
    down_received = delivered
    down_status = (f"downlink benchmark bytes={down_received} "
                   f"skipped={down_skipped} errors={down_corrupt}")
    down_loss = max(0, sent - down_received)

    source_rate = source * 8 / args.seconds
    uplink_rate = received * 8 / args.seconds
    downlink_rate = down_received * 8 / elapsed
    print(down_status)
    print(f"uplink: target {args.uplink / 1e6:.3f} Mb/s, "
          f"source {source_rate / 1e6:.3f} Mb/s, "
          f"received {uplink_rate / 1e6:.3f} Mb/s, "
          f"gaps {gaps}, corrupt {corrupt}")
    print(f"downlink: target {args.downlink / 1e6:.3f} Mb/s, "
          f"sent {sent * 8 / elapsed / 1e6:.3f} Mb/s, "
          f"delivered {downlink_rate / 1e6:.3f} Mb/s, "
          f"loss {down_loss * 100 / sent:.2f}%, "
          f"skipped {down_skipped}, corrupt {down_corrupt}")

    passed = (uplink_rate >= args.uplink * 0.95 and
              downlink_rate >= args.downlink * 0.95 and
              corrupt == 0 and down_corrupt == 0)
    print(f"RESULT: {'PASS' if passed else 'FAIL'}")
    if not passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
