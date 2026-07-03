# K210_ESP_SPI_WIFI

ESP8285 firmware for the Wi-Fi/SPI bridge used by `K210_AI_V7s_Plus`.

Goal: build the ESP8285 firmware on Windows, send it through the already-running ESP/K210 Wi-Fi/SPI bridge, write it to the K210 SD card, and then let K210 flash the ESP8285 from SD on next reboot.

## How the flashing chain works

```text
Windows PC
  |
  | TCP PUT, port 7777
  v
ESP8285 bridge firmware from this repo
  |
  | SPI frames, 32 bytes each
  v
K210_AI_V7s_Plus firmware
  |
  | writes files to SD: esp_boot.bin, esp_irom.bin, flash.json
  v
K210 reboot
  |
  | flash.json enables one-shot ESP flashing
  v
K210 flashes ESP8285 through UART2 + EN/BOOT pins
```

`flash.json` is disarmed by K210 before flashing, so the ESP is not flashed again on every boot.

## One-time Windows setup

From the repository folder:

```bat
python -m pip install -r requirements.txt
```

If Python is missing, install current Python for Windows and enable `Add python.exe to PATH` during installation.

## Normal flashing command

1. Flash/run the current `K210_AI_V7s_Plus` firmware on the K210 module.
2. Wait until the ESP bridge prints an IP in the serial log, for example:

```text
kesp: ip=192.168.1.123 status=3 spi=1 rx=0 B/s
```

3. Run from this repo:

```bat
flash_esp_via_k210.bat --host 192.168.1.123 --monitor COM7
```

Replace:

- `192.168.1.123` with the IP printed by the ESP bridge or shown by your router.
- `COM7` with the K210 USB/CH340 COM port.

After upload, the script will ask you to reboot/power-cycle K210 and press Enter. Then it captures K210 serial log and waits for ESP flashing result.

## If you only want to prepare files, without Wi-Fi upload

```bat
flash_esp_via_k210.bat --dry-run
```

Generated files will be in:

```text
out/flash_payload/
  esp_boot.bin
  esp_irom.bin
  flash.json
```

You can copy these files manually to the K210 SD card root. Then reboot K210.

## If you already have a ready `.bin`

```bat
flash_esp_via_k210.bat --host 192.168.1.123 --firmware firmware\esp8285_at.bin --remote-name esp8285_at.bin --offset 0x0 --monitor COM7
```

This sends one image and creates `flash.json` with one part at offset `0x00000000`.

## What log to send back

Send the newest file from:

```text
logs\flash_payload_*.log
```

Also useful: copy the K210 serial console if it shows `[esp-flash]` errors.

Good signs:

```text
kesp: spi slave ready
kesp: ip=... status=3 spi=1 ...
[wifi-spi] BEGIN flash.json ...
[esp-flash] config flash_once ESP job found
[esp-flash] connected target=ESP8266/ESP8285
[esp-flash] progress ...
ESP flash result: OK
```

Bad signs to send here:

```text
spi=0
[wifi-spi] does not show BEGIN/END
[esp-flash] image missing
[esp-flash] connect failed
[esp-flash] write failed
ESP flash result: FAIL
```

## Repo files

```text
platformio.ini                 PlatformIO ESP8285 project
src/main.cpp                   ESP8285 Wi-Fi/TCP to SPI bridge
tools/merge_image.py           PlatformIO post-build helper
tools/send_flash_payload.py    Windows/Python sender + log collector
flash_esp_via_k210.bat         one-command Windows entry point
requirements.txt               Python tools for helper script
```
