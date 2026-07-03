# ESP8285 hello_uart on official ESP8266_RTOS_SDK v3.4

This is a clean bring-up project for ESP8285 / ESP8266 without Arduino and without PlatformIO.

Goal of this step:

1. Build a minimal RTOS SDK firmware with `app_main()`.
2. Prove that the official ESP8266_RTOS_SDK toolchain works on Windows.
3. Later flash it directly to ESP8285 UART0 or through K210 only after the build is proven.
4. See stable UART log at 115200 baud.

This project intentionally does not contain SPI slave, Wi-Fi, TCP, SD or K210 flashing logic yet.

## Why the first build looked huge

It was not compiling the toolchain. The toolchain is already downloaded as `xtensa-lx106-elf-gcc.exe`.

The old ESP8266_RTOS_SDK Make build compiles SDK components into local project libraries on the first build. Without a component limit it starts building many optional components: HTTP server/client, MQTT, mbedTLS, libsodium, lwIP extras, Modbus, FatFS, etc.

For this hello bring-up the project Makefile now limits the component set with `COMPONENTS := ...` so a clean build stays focused on the minimum core plus ESP8266 driver/HAL, Wi-Fi/SPI base, image tools and partition/flash support.

## SDK location expected by helper scripts

```text
D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK
```

The SDK must be Espressif `ESP8266_RTOS_SDK` tag `v3.4`.

## Required MSYS2 packages

The build helper runs `tools/esp8266_rtos_msys_setup.sh` before building. It installs only package names that exist in the plain MSYS repo used by `C:\msys64\usr\bin\bash.exe`:

```bash
pacman -S --needed --noconfirm \
  bash git make diffutils patch tar gzip unzip wget curl \
  python python-setuptools python-pip python-packaging \
  cmake ninja flex bison gperf gettext-devel ncurses-devel libexpat
```

The setup script smoke-tests only the Python core module needed before build:

```python
import pyexpat
```

`ESP8266_RTOS_SDK v3.4` still imports `pkg_resources` in `tools/check_python_dependencies.py`. Modern MSYS Python 3.12 may not provide that module even when `python-setuptools` is installed, so the build helper adds this local shim to `PYTHONPATH` before running `make`:

```text
tools/python_shims/pkg_resources.py
```

`esptool.py` also imports `serial.tools.list_ports` even when Make only needs to generate `.bin` images. For build-only image generation the helper uses this local shim:

```text
tools/python_shims/serial/
```

The serial shim is not enough for real flashing. Direct ESP flashing needs real pyserial, or we use the K210 flashing path later.

The ESP8266 xtensa toolchain is separate and is downloaded by the Windows `.bat` helper into:

```text
D:\w_space\esp8266_sdk\xtensa-lx106-elf
```

Important: the `.bat` helper does **not** prepend the xtensa toolchain to PATH before Python checks, because toolchain DLLs can shadow MSYS Python DLL dependencies and break `pyexpat`.

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
