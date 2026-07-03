"""Minimal pyserial shim for ESP8266_RTOS_SDK build-only image generation.

ESP8266_RTOS_SDK v3.4 vendors esptool.py. Even when Make only needs
elf2image/bin generation, esptool imports serial.tools.list_ports at module load.
On this MSYS setup pyserial is not available as an MSYS package. This shim is
enough for build-only image generation.

It is not enough for actual flashing. Direct ESP flashing must use real pyserial
or the K210 flashing path.
"""

VERSION = "build-only-shim"
PARITY_NONE = "N"
STOPBITS_ONE = 1
EIGHTBITS = 8


class SerialException(Exception):
    pass


class SerialTimeoutException(SerialException):
    pass


class Serial:
    def __init__(self, *args, **kwargs):
        raise SerialException("serial shim is build-only; real pyserial is required for flashing")


def serial_for_url(*args, **kwargs):
    raise SerialException("serial shim is build-only; real pyserial is required for flashing")
