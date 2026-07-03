#!/usr/bin/env bash
set -euo pipefail

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
        echo "Using python3 through local python shim: $(command -v python)"
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

ensure_python_pkg_resources()
{
    if python - <<'PY'
import pkg_resources
PY
    then
        return 0
    fi

    echo "Python pkg_resources not found. Installing MSYS python-setuptools..."
    if command -v pacman >/dev/null 2>&1; then
        pacman -S --needed --noconfirm python-setuptools
    fi

    if python - <<'PY'
import pkg_resources
PY
    then
        return 0
    fi

    echo "ERROR: Python pkg_resources is still missing."
    echo "Try manually in MSYS2: pacman -S --needed python python-setuptools"
    exit 4
}

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

ensure_python_cmd
ensure_python_pkg_resources
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

# Keep this first build-only test independent from pip: on some MSYS installs
# Python 3.12 pip fails while importing pyexpat. The hello build should prove
# the compiler/SDK path first. If make later reports a missing Python package,
# that will be handled as the next layer.
if [ "${ESP8266_RTOS_INSTALL_REQS:-0}" = "1" ] && [ -f "$SDK/requirements.txt" ]; then
    python -m pip install --user -r "$SDK/requirements.txt"
else
    echo "Skipping pip requirements install for build-only bring-up."
fi

cd "$PROJ"
make defconfig
make -j"${NUMBER_OF_PROCESSORS:-4}"

echo
echo "OK: hello_uart build finished."
echo "Build directory: $PROJ/build"
