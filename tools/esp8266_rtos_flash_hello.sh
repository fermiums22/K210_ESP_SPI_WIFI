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

cd "$SDK"

if ! command -v python >/dev/null 2>&1; then
    if command -v python3 >/dev/null 2>&1; then
        mkdir -p "$PROJ/.local-tools"
        ln -sf "$(command -v python3)" "$PROJ/.local-tools/python"
        export PATH="$PROJ/.local-tools:$PATH"
    else
        echo "ERROR: python/python3 not found in this MSYS shell."
        echo "Run once in MSYS2 shell: pacman -S --needed python"
        exit 2
    fi
fi

./install.sh
. ./export.sh

cd "$PROJ"
make defconfig
make -j"${NUMBER_OF_PROCESSORS:-4}"
make erase_flash flash ESPPORT="$ESPPORT" ESPBAUD="$ESPBAUD"

echo
echo "OK: flashed hello_uart to $ESPPORT at $ESPBAUD."
echo "Now reset ESP to normal boot mode and open UART log at 115200."
