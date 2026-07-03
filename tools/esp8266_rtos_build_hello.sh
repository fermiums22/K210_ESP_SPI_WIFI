#!/usr/bin/env bash
set -euo pipefail

SDK="/d/w_space/esp8266_sdk/ESP8266_RTOS_SDK"
PROJ="/d/w_space/K210_ESP_SPI_WIFI/esp8266_rtos_clean/hello_uart"

cd "$SDK"

echo "=== ESP8266_RTOS_SDK hello build ==="
echo "SDK : $SDK"
echo "PROJ: $PROJ"
echo "PWD : $(pwd)"
echo

if ! command -v python >/dev/null 2>&1; then
    if command -v python3 >/dev/null 2>&1; then
        mkdir -p "$PROJ/.local-tools"
        ln -sf "$(command -v python3)" "$PROJ/.local-tools/python"
        export PATH="$PROJ/.local-tools:$PATH"
        echo "Using python3 through local python shim: $(command -v python)"
    else
        echo "ERROR: python/python3 not found in this MSYS shell."
        echo "Run once in MSYS2 shell: pacman -S --needed python"
        exit 2
    fi
fi

python --version
make --version | head -n 1

# The v3.4 SDK has an obsolete nested tinydtls URL under the optional CoAP module.
# hello_uart does not use CoAP, so do not block bring-up on that nested submodule.
# Keep already downloaded useful submodules, but avoid recursive repair here.
git submodule update --init components/json/cJSON components/lwip/lwip components/mbedtls/mbedtls components/mqtt/esp-mqtt || true

./install.sh
. ./export.sh

xtensa-lx106-elf-gcc --version | head -n 1

cd "$PROJ"
make defconfig
make -j"${NUMBER_OF_PROCESSORS:-4}"

echo
echo "OK: hello_uart build finished."
echo "Build directory: $PROJ/build"
