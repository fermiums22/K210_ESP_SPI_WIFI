#!/usr/bin/env bash
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: tools/esp8266_rtos_flash_hello.sh COMxx [baud]"
    echo "Example: tools/esp8266_rtos_flash_hello.sh COM13 460800"
    exit 2
fi

ESPPORT="$1"
ESPBAUD="${2:-460800}"
SDK="/d/w_space/esp8266_sdk/ESP8266_RTOS_SDK"
PROJ="/d/w_space/K210_ESP_SPI_WIFI/esp8266_rtos_clean/hello_uart"
TOOLCHAIN_BIN="/d/w_space/esp8266_sdk/xtensa-lx106-elf/bin"

ensure_python_cmd()
{
    if command -v python >/dev/null 2>&1; then
        return 0
    fi

    if command -v python3 >/dev/null 2>&1; then
        mkdir -p "$PROJ/.local-tools"
        ln -sf "$(command -v python3)" "$PROJ/.local-tools/python"
        export PATH="$PROJ/.local-tools:$PATH"
        return 0
    fi

    if command -v pacman >/dev/null 2>&1; then
        echo "python/python3 not found in MSYS. Installing MSYS python with pacman..."
        pacman -S --needed --noconfirm python
        if command -v python >/dev/null 2>&1; then
            return 0
        fi
    fi

    echo "ERROR: python/python3 not found in this MSYS shell."
    echo "Run once in MSYS2 shell: pacman -S --needed python"
    exit 2
}

ensure_toolchain_cmd()
{
    export PATH="$TOOLCHAIN_BIN:$PATH"

    if command -v xtensa-lx106-elf-gcc >/dev/null 2>&1; then
        return 0
    fi

    echo "ERROR: xtensa-lx106-elf-gcc not found."
    echo "Expected toolchain bin: $TOOLCHAIN_BIN"
    exit 3
}

cd "$SDK"
ensure_python_cmd
ensure_toolchain_cmd

export IDF_PATH="$SDK"
export IDF_PYTHON_ENV_PATH=""

# Avoid automatic pip install by default. If make flash reports missing pyserial
# or esptool dependencies, rerun with ESP8266_RTOS_INSTALL_REQS=1 after fixing MSYS pip.
if [ "${ESP8266_RTOS_INSTALL_REQS:-0}" = "1" ] && [ -f "$SDK/requirements.txt" ]; then
    python -m pip install --user -r "$SDK/requirements.txt"
else
    echo "Skipping pip requirements install for direct flash bring-up."
fi

cd "$PROJ"
make defconfig
make -j"${NUMBER_OF_PROCESSORS:-4}"
make erase_flash flash ESPPORT="$ESPPORT" ESPBAUD="$ESPBAUD"

echo
echo "OK: flashed hello_uart to $ESPPORT at $ESPBAUD."
echo "Now reset ESP to normal boot mode and open UART log at 115200."
