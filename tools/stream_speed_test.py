#!/usr/bin/env python3
import argparse
import socket
import struct
import threading
import time


def pattern(offset):
    return (0x6D ^ offset ^ (offset >> 9) ^ (offset >> 21)) & 0xFF


def console_command(sock, command):
    sock.sendall((command + "\n").encode())
    deadline = time.time() + 2
    data = bytearray()
    while time.time() < deadline:
        try:
            data.extend(sock.recv(4096))
        except socket.timeout:
            continue
        if b"\n" in data:
            break
    return data.decode("ascii", "replace").strip()


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

    console = socket.create_connection((args.host, 21012), 5)
    console.settimeout(0.2)
    banner = bytearray()
    while b"SPI stage=" not in banner:
        banner.extend(console.recv(4096))
    print(console_command(console, "bench on"))
    print(console_command(console, "bench down reset"))

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

    down_status = console_command(console, "bench down off")
    console_command(console, "bench off")
    console.close()
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

    emitted = 0 if first_offset is None else last_offset - first_offset
    print(down_status)
    print(f"uplink: emitted {emitted * 8 / args.seconds / 1e6:.3f} Mb/s, "
          f"received {received * 8 / args.seconds / 1e6:.3f} Mb/s, "
          f"gaps {gaps}, corrupt {corrupt}")
    print(f"downlink: sent {sent * 8 / elapsed / 1e6:.3f} Mb/s")


if __name__ == "__main__":
    main()
