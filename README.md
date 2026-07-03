# K210_ESP_SPI_WIFI

ESP8285 bring-up repository for the K210 + ESP8285 link.

Current branch policy: **use `main` only**. Old experiment branches are not part of the normal workflow anymore.

## Current actual status

The current working state is intentionally small and repeatable:

1. ESP8285 firmware is built with **official Espressif ESP8266_RTOS_SDK v3.4**.
2. Build is started from normal Windows `cmd.exe`, but the actual ESP build runs through **MSYS2**.
3. Current ESP application is `hello_uart`:
   - boots under RTOS SDK;
   - prints SDK/chip/flash info;
   - prints `alive seq=...` every second on UART at 115200 baud.
4. K210 is used as a diagnostic loader:
   - PC talks to K210 over COM12 / 921600 using the KSD protocol;
   - PC writes ESP flash payload files to the K210 SD card;
   - PC sends `FLASH_ESP`;
   - K210 flashes ESP8285 over ESP UART/BOOT/EN pins;
   - K210 then bridges ESP UART logs back to PC.

This is the stable checkpoint before implementing the final RTOS Wi-Fi/SPI bridge.

## One-command test

From Windows `cmd.exe`:

```bat
cd /d D:\w_space\K210_AI_V7s_Plus && git fetch origin --prune && git checkout main && git pull --ff-only origin main && build_k210.bat && flash_k210.bat COM12 --no-build && cd /d D:\w_space\K210_ESP_SPI_WIFI && git fetch origin --prune && git checkout main && git pull --ff-only origin main && run_esp8285_rtos_hello_via_k210.bat COM12
```

Expected ESP log after successful flash and boot:

```text
BOOT: ESP8285 / ESP8266 RTOS SDK hello_uart
SDK version: ...
Flash size=1048576 bytes (1 MB)
alive seq=0 tick=...
alive seq=1 tick=...
```

## Build ESP RTOS hello only

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && git fetch origin --prune && git checkout main && git pull --ff-only origin main && run_esp8285_rtos_hello_build.bat
```

Expected result:

```text
OK: ESP8285 RTOS SDK hello build finished.
```

Build outputs are generated locally under:

```text
esp8266_rtos_clean\hello_uart\build\
```

Important files:

```text
bootloader\bootloader.bin              -> offset 0x00000000
partitions_1mb_singleapp.bin           -> offset 0x00008000
esp8285-hello-uart.bin                 -> offset 0x00010000
```

## Flash ESP RTOS hello through K210

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && run_esp8285_rtos_hello_via_k210.bat COM12
```

This command:

1. builds ESP RTOS hello through MSYS2;
2. opens K210 KSD service on COM12 / 921600;
3. writes ESP payload files to the K210 SD card;
4. sends `FLASH_ESP`;
5. waits for ESP RTOS hello UART log.

## Why Arduino path was stopped

Arduino was useful only as a very quick early Wi-Fi/TCP/SPI experiment.

We stopped using it because:

- the target project must use a normal RTOS SDK, not Arduino;
- ESP8266 Arduino `SPISlave` showed unstable/laggy SPI behavior for this use case;
- keeping Arduino in the repo started to hide real RTOS bring-up problems;
- it made the flow look more complete than it actually was.

Therefore Arduino source was removed from the normal `main` flow.

## Why PlatformIO RTOS path was stopped

PlatformIO was also tested as an ESP8266 RTOS build wrapper.

We stopped using it because:

- the ESP8266 RTOS support in PlatformIO is old and unclear;
- the exact toolchain/package mapping is not transparent enough for a repeatable product workflow;
- it mixed `.pio` artifacts and framework assumptions into the repo;
- it was hard to reason about which SDK/toolchain version was actually used.

The selected direction is now explicit:

```text
Windows cmd -> C:\msys64\usr\bin\bash.exe -> official ESP8266_RTOS_SDK v3.4 -> xtensa-lx106-elf gcc8_4_0
```

## What was achieved

- K210 side can be built/flashed from `main`.
- K210 command firmware has persistent KSD service.
- K210 no longer waits through a long SD mount retry loop.
- ESP official RTOS SDK toolchain can build a real ESP8285 app from MSYS2.
- ESP flash artifacts are known and mapped to explicit offsets.
- A K210-based upload path exists for flashing those RTOS artifacts through K210.

## What is not implemented yet

The final target is still:

```text
PC -> Wi-Fi -> ESP8285 RTOS -> SPI -> K210 -> SD read/write
```

This is **not implemented yet** on the selected official RTOS SDK path.

Next real development step:

1. add Wi-Fi station mode in ESP8266_RTOS_SDK;
2. add TCP PUT server;
3. add RTOS SPI slave transfer layer;
4. keep the K210 SPI scanner/receiver simple and measurable;
5. only then return to PC -> Wi-Fi -> SPI -> SD throughput tests.

## Repository map

```text
esp8266_rtos_clean/hello_uart/          Current official RTOS SDK hello project
run_esp8285_rtos_hello_build.bat        Build ESP RTOS hello through MSYS2
run_esp8285_rtos_hello_via_k210.bat     Build + upload + flash via K210 KSD
tools/esp8266_rtos_build_hello.sh       MSYS build helper
tools/esp8266_rtos_msys_setup.sh        MSYS/Python dependency setup
tools/upload_rtos_hello_via_k210.py     K210 KSD upload helper
tools/send_flash_payload*.py            Shared KSD payload helpers
out/                                    Generated payload files, ignored
logs/                                   Generated logs, ignored
```
