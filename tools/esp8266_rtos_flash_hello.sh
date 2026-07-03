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
SETUP_SCRIPT="/d/w_space/K210_ESP_SPI_WIFI/tools/esp8266_rtos_msys_setup.sh"

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
bash "$SETUP_SCRIPT"
ensure_toolchain_cmd

export IDF_PATH="$SDK"
export IDF_PYTHON_ENV_PATH=""

cd "$PROJ"
make defconfig
make -j"${NUMBER_OF_PROCESSORS:-4}"
make erase_flash flash ESPPORT="$ESPPORT" ESPBAUD="$ESPBAUD"

echo
echo "OK: flashed hello_uart to $ESPPORT at $ESPBAUD."
echo "Now reset ESP to normal boot mode and open UART log at 115200."
