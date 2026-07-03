#!/usr/bin/env bash
set -euo pipefail

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
    echo "The Windows BAT should download and unpack xtensa-lx106-elf-gcc8_4_0 before entering this script."
    exit 3
}

cd "$SDK"

echo "=== ESP8266_RTOS_SDK hello build ==="
echo "SDK : $SDK"
echo "PROJ: $PROJ"
echo "PWD : $(pwd)"
echo

bash "$SETUP_SCRIPT"
ensure_toolchain_cmd

export IDF_PATH="$SDK"
export IDF_PYTHON_ENV_PATH=""

python --version
make --version | head -n 1
xtensa-lx106-elf-gcc --version | head -n 1

# The v3.4 SDK has an obsolete nested tinydtls URL under the optional CoAP module.
# hello_uart does not use CoAP, so do not block bring-up on that nested submodule.
# Keep already downloaded useful submodules, but avoid recursive repair here.
git submodule update --init components/json/cJSON components/lwip/lwip components/mbedtls/mbedtls components/mqtt/esp-mqtt || true

echo "Building hello_uart..."
cd "$PROJ"
make defconfig
make -j"${NUMBER_OF_PROCESSORS:-4}"

echo
echo "OK: hello_uart build finished."
echo "Build directory: $PROJ/build"
