# K210_ESP_SPI_WIFI

ESP8285 bring-up repository for the K210 + ESP8285 link.

Current branch policy: **use `main` only**. Old experiment branches are not part of the normal workflow anymore.

## Mandatory cross-repo notes

The full PC -> Wi-Fi -> ESP -> SPI -> K210 -> SD verifier depends on the K210 SD backend.
Before changing ESP-side full-flow scripts because of `KSD:SD_FAIL rw`, read the K210 SD/SPI notes first:

```text
D:\w_space\K210_AI_V7s_Plus\docs\SD_SPI_K210_REGISTER_MAP.md
```

Current known K210-side blocker:

```text
[sdcard] CMD0 r=ff
KSD:SD_FAIL rw
```

This means the K210 does not receive a response to the first SD SPI-mode CMD0 on its current SD init path.
Do not debug ESP Wi-Fi, TCP, SPI framing, SHA verification, or payload size while this K210 SD smoke test fails.
The expected first K210-side recovery milestone is:

```text
[sdcard] CMD0 r=01
```

## Main goal

The target development cycle is:

```text
PC -> Wi-Fi -> ESP8285 RTOS -> SPI -> K210 -> SD write -> KSD GET -> byte-for-byte verify
```

This repository owns the ESP8285 RTOS firmware and the PC-side full-flow scripts.
The sibling K210 repository owns the K210 firmware, KSD console, SD service, ESP flashing bridge, and SPI receiver:

```text
D:\w_space\K210_AI_V7s_Plus
D:\w_space\K210_ESP_SPI_WIFI
```

## Current actual status

The current working path is now a strict one-pass hardware test:

1. K210 is built and flashed from `K210_AI_V7s_Plus/main`.
2. Optional local fast tuning is applied to K210 before build:
   - KSD PUT/GET chunk: 4096 bytes;
   - KSD task stack: 12288;
   - ESP flashing baud: 230400;
   - ESP flashing block: 4096;
   - KSD file PUT/GET uses direct FatFs in the fast-tuned build.
3. ESP8285 firmware is built with **official Espressif ESP8266_RTOS_SDK v3.4**.
4. PC talks to K210 over COM12 / 921600 using KSD.
5. PC uploads ESP bootloader, partition table, app slices, Wi-Fi config, and `flash.json` to the K210 SD card.
6. PC sends `FLASH_ESP`.
7. K210 flashes ESP8285 over ESP UART/BOOT/EN.
8. PC sends `RUN_SPI`.
9. ESP8285 boots RTOS firmware, joins Wi-Fi / starts fallback AP, starts TCP server.
10. PC sends a binary file to ESP TCP.
11. ESP sends that file over SPI to K210.
12. K210 writes it to SD as `wifi/pc_wifi_spi_sd.bin`.
13. PC reads the file back through KSD `GET` and verifies SHA256.

## Full strict test from Windows cmd.exe

Use this command from normal Windows `cmd.exe`.

It intentionally uses `git fetch` + `git switch -f` + `git reset --hard origin/main` instead of `git pull`, so the local workspace is repeatable.

```bat
cd /d D:\w_space\K210_AI_V7s_Plus && git fetch origin main && git switch -f main && git reset --hard origin/main && py -3 tools\apply_fast_io_tuning.py --ksd-buf 4096 --ksd-stack 12288 --esp-baud 230400 --esp-block 4096 && call build_k210.bat && call flash_k210.bat COM12 --no-build && cd /d D:\w_space\K210_ESP_SPI_WIFI && git fetch origin main && git switch -f main && git reset --hard origin/main && call run_pc_wifi_spi_sd_full_test.bat COM12 65536
```

Expected K210 tuning line:

```text
FAST_IO_TUNING_OK ksd_buf=4096 ksd_stack=12288 esp_baud=230400 esp_block=4096 ksd_io=fatfs
```

Expected K210 build result:

```text
OK: D:\w_space\K210_AI_V7s_Plus\build\robot_show.bin
```

Expected K210 KSD console/read-write smoke:

```text
KSD:GO 4096
[ksd] PUT OK bytes=4096 chunk=4096 acks=1
PASS KSD_CONSOLE_RW path=ksd_console_probe.bin size=4096 sha256=...
```

Expected ESP build result:

```text
OK: ESP8285 RTOS SDK hello build finished.
```

Expected ESP payload upload checkpoint:

```text
PUT esp_010000.bin OK bytes=262144 chunk=4096 acks=64
PUT esp_050000.bin OK bytes=184592 chunk=4096 acks=46
```

Expected ESP flashing checkpoint:

```text
[esp-flash] connected target=ESP8266/ESP8285
[esp-flash] session baud=230400 block=4096
KSD:FLASH_OK
```

Expected final result:

```text
PASS PC_WIFI_SPI_SD path=wifi/pc_wifi_spi_sd.bin size=65536 sha256=...
```

## Individual commands

### Build K210 only

```bat
cd /d D:\w_space\K210_AI_V7s_Plus && git fetch origin main && git switch -f main && git reset --hard origin/main && py -3 tools\apply_fast_io_tuning.py --ksd-buf 4096 --ksd-stack 12288 --esp-baud 230400 --esp-block 4096 && call build_k210.bat
```

### Flash K210 only after successful build

```bat
cd /d D:\w_space\K210_AI_V7s_Plus && call flash_k210.bat COM12 --no-build
```

### K210 KSD console/read-write smoke only

```bat
cd /d D:\w_space\K210_AI_V7s_Plus && py -3 tools\ksd_console_rw_test.py --port COM12 --baud 921600 --size 4096
```

Expected:

```text
PASS KSD_CONSOLE_RW path=ksd_console_probe.bin size=4096 sha256=...
```

### Build ESP RTOS hello only

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && git fetch origin main && git switch -f main && git reset --hard origin/main && call run_esp8285_rtos_hello_build.bat
```

Expected:

```text
OK: ESP8285 RTOS SDK hello build finished.
```

### Build + upload + flash ESP via K210 only

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && git fetch origin main && git switch -f main && git reset --hard origin/main && call run_esp8285_rtos_hello_via_k210.bat COM12
```

### Full PC -> Wi-Fi -> ESP -> SPI -> K210 -> SD test only

Run this only after K210 is already flashed with the matching build.

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && call run_pc_wifi_spi_sd_full_test.bat COM12 65536
```

## Important logs and what they mean

| Log line | Meaning |
|---|---|
| `KSD:GO 4096` | K210 KSD is using the fast 4096-byte binary chunk. |
| `PASS KSD_CONSOLE_RW ...` | K210 console, SD mount, PUT, GET, and SHA verify work. |
| `PUT esp_010000.bin OK ...` | ESP app slice was uploaded to SD through KSD. |
| `[esp-flash] Connected - target: ESP8266` | K210 can put ESP8285 into serial bootloader mode. |
| `[esp-flash] session baud=230400 block=4096` | Fast ESP flash settings are active. |
| `KSD:FLASH_OK` | ESP8285 flashing completed. |
| `[wifi-sd] VERDICT SPI_OK ...` | K210 found a valid KESP SPI frame from ESP8285. |
| `[wifi-sd] WIFI_SD_OK wifi/pc_wifi_spi_sd.bin ...` | K210 received the TCP payload over SPI and wrote it to SD. |
| `PASS PC_WIFI_SPI_SD ...` | Full target path passed with byte-for-byte SHA verification. |

## Known failure meanings

| Failure | Meaning / next action |
|---|---|
| `KSD:SD_FAIL rw` with `[sdcard] CMD0 r=ff` | K210 SD init did not receive a response to the first SD SPI-mode command. Read `D:\w_space\K210_AI_V7s_Plus\docs\SD_SPI_K210_REGISTER_MAP.md`; this is a K210 SD/SPI/FPIOA/GPIOHS/LCD-bus issue, not ESP. |
| `KSD:SD_FAIL rw` with `sdcard init rc=255 capacity=0 block=0` | SD driver returned a bad card state immediately after fresh K210 reboot. Update `K210_AI_V7s_Plus/main`; `sd.c` now drops the bad handle and reinitializes SD/SPI/GPIO before declaring failure. |
| `GET size: KSD:MISSING` immediately after KSD `PUT OK` | KSD PUT/GET must use direct FatFs, not VFS. Run the full command with the latest `apply_fast_io_tuning.py`; expected line includes `ksd_io=fatfs`. |
| `FLASH_ESP monitor: no line from K210` | Old PC monitor timeout. Update `K210_ESP_SPI_WIFI/main`; current scripts use blocking serial reads after KSD connect. |
| `write failed at part ...` during ESP flashing | ESP serial flashing link failed mid-transfer. Use fast tuning first: `--esp-baud 230400 --esp-block 4096`. If it still fails, test a lower baud but keep block 4096. |
| `[wifi-sd] VERDICT SPI_FAIL` | ESP booted but K210 did not find valid KESP SPI frames. Check SPI pins/mode/CS scan output. |
| `WIFI_SD_FAIL` or `DATA offset mismatch` | SPI transport corrupted or lost a frame. Keep payload small, inspect K210 `[wifi-sd]` logs. |

## ESP build path

The selected direction is explicit:

```text
Windows cmd -> C:\msys64\usr\bin\bash.exe -> D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK -> xtensa-lx106-elf gcc8_4_0
```

Do not use Arduino SDK or PlatformIO for this flow.

## Repository map

```text
esp8266_rtos_clean/hello_uart/          Official ESP8266_RTOS_SDK app
run_esp8285_rtos_hello_build.bat        Build ESP RTOS app through MSYS2
run_esp8285_rtos_hello_via_k210.bat     Build + upload + flash via K210 KSD
run_pc_wifi_spi_sd_full_test.bat         Full strict PC -> Wi-Fi -> SPI -> SD verifier
tools/upload_rtos_hello_via_k210.py     KSD ESP payload uploader and FLASH_ESP monitor
tools/pc_wifi_spi_sd_rw_test.py         TCP PUT + KSD GET SHA verifier
tools/esp8266_rtos_build_hello.sh       MSYS build helper
tools/esp8266_rtos_msys_setup.sh        MSYS/Python dependency setup
out/                                    Generated payload files, ignored
logs/                                   Generated logs, ignored
```
