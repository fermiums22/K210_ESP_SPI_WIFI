#!/usr/bin/env bash
set -euo pipefail

SDK="/d/w_space/esp8266_sdk/ESP8266_RTOS_SDK"
PROJ="/d/w_space/K210_ESP_SPI_WIFI/esp8266_rtos_clean/hello_uart"
TOOLCHAIN_BIN="/d/w_space/esp8266_sdk/xtensa-lx106-elf/bin"
SETUP_SCRIPT="/d/w_space/K210_ESP_SPI_WIFI/tools/esp8266_rtos_msys_setup.sh"
PY_SHIMS="/d/w_space/K210_ESP_SPI_WIFI/tools/python_shims"
PY_VENV_BIN="/d/w_space/K210_ESP_SPI_WIFI/.local-tools/esp8266_py/bin"

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
export PATH="$PY_VENV_BIN:$PATH"
ensure_toolchain_cmd

export IDF_PATH="$SDK"
export IDF_PYTHON_ENV_PATH=""
export PYTHONPATH="$PY_SHIMS${PYTHONPATH:+:$PYTHONPATH}"

python --version
make --version | head -n 1
xtensa-lx106-elf-gcc --version | head -n 1
python - <<'PY'
import pyparsing
import pkg_resources
pkg_resources.require('setuptools')
import serial.tools.list_ports
print('python venv/shim smoke test OK')
PY

# The v3.4 SDK has an obsolete nested tinydtls URL under the optional CoAP module.
# hello_uart does not use CoAP, so do not block bring-up on that nested submodule.
# Keep already downloaded useful submodules, but avoid recursive repair here.
for d in components/json/cJSON components/lwip/lwip components/mbedtls/mbedtls components/mqtt/esp-mqtt; do
    if [ ! -e "$SDK/$d" ] || [ -z "$(ls -A "$SDK/$d" 2>/dev/null || true)" ]; then
        git submodule update --init "$d" || true
    fi
done

echo "Building hello_uart incrementally..."
cd "$PROJ"
if [ "${ESP8266_RTOS_FORCE_REBUILD:-0}" = "1" ]; then
    echo "ESP8266_RTOS_FORCE_REBUILD=1 -> removing build directory and sdkconfig"
    rm -rf build sdkconfig
fi

if [ ! -f sdkconfig ]; then
    make defconfig
fi

make -j"${NUMBER_OF_PROCESSORS:-4}"

echo
echo "OK: hello_uart build finished."
echo "Build directory: $PROJ/build"
