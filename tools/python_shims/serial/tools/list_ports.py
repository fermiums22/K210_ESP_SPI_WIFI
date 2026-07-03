"""Minimal serial.tools.list_ports shim for build-only esptool import."""


def comports(*args, **kwargs):
    return []
