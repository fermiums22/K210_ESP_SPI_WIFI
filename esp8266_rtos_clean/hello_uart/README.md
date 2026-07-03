# ESP8285 hello_uart on official ESP8266_RTOS_SDK v3.4

This is a clean bring-up project for ESP8285 / ESP8266 without Arduino and without PlatformIO.

Goal of this step:

1. Build a minimal RTOS SDK firmware with `app_main()`.
2. Prove that the official ESP8266_RTOS_SDK toolchain works on Windows.
3. Later flash it directly to ESP8285 UART0 or through K210 only after the build is proven.
4. See stable UART log at 115200 baud.

This project intentionally does not contain SPI slave, Wi-Fi, TCP, SD or K210 flashing logic yet.

## SDK location expected by helper scripts

```text
D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK
```

The SDK must be Espressif `ESP8266_RTOS_SDK` tag `v3.4`.

## Required MSYS2 packages

The build helper runs `tools/esp8266_rtos_msys_setup.sh` before building. It installs the full known package set in one step instead of discovering missing packages one by one:

```bash
pacman -S --needed --noconfirm \
  bash git make diffutils patch tar gzip unzip wget curl \
  python python-setuptools python-pip python-wheel python-packaging \
  python-pyserial python-click python-cryptography python-pyparsing \
  python-pyelftools python-future \
  cmake ninja flex bison gperf gettext-devel ncurses-devel libexpat
```

The setup script also smoke-tests these Python imports:

```python
import pyexpat
import pkg_resources
import serial
import click
import cryptography
import pyparsing
import elftools
import future
```

This avoids relying on `./install.sh`, which rejects current MSYS64 as an unsupported platform, and avoids iterative package guessing.

## Build-only test from Windows cmd

From normal Windows `cmd.exe`:

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && git fetch origin && git checkout spi-uart-test && git pull --ff-only origin spi-uart-test && run_esp8285_rtos_hello_build.bat
```

This build-only test does not use K210 COM12.

Expected successful result:

```text
OK: hello_uart build finished.
Build directory: /d/w_space/K210_ESP_SPI_WIFI/esp8266_rtos_clean/hello_uart/build
```

## Direct ESP flash test

Only use this when ESP UART0, GPIO0 and reset/CHIP_EN are available directly from the PC USB-UART adapter.

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && run_esp8285_rtos_hello_flash_direct.bat COM13 460800
```

Replace `COM13` with the real direct ESP UART port.

Do not use K210 service UART COM12 for this direct flash command.

Expected UART log after successful flashing and normal ESP boot:

```text
I (...) esp8285_hello: BOOT: ESP8285 / ESP8266 RTOS SDK hello_uart
I (...) esp8285_hello: SDK version: ...
I (...) esp8285_hello: Flash size=1048576 bytes (1 MB)
I (...) esp8285_hello: alive seq=0 tick=...
I (...) esp8285_hello: alive seq=1 tick=...
```

## Notes about the SDK clone log

The old nested CoAP/tinydtls submodule URL may fail because it redirects from the old Eclipse Git host. The hello_uart project does not use CoAP, so the build helper intentionally does not block on repairing that nested submodule.
