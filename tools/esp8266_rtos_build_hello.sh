#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SDK="${IDF_PATH:-}"
if [ -z "$SDK" ]; then
    for base in /c/ESP8266/sdk /d/w_space/esp8266_sdk; do
        if [ -f "$base/ESP8266_RTOS_SDK/make/project.mk" ]; then
            SDK="$base/ESP8266_RTOS_SDK"
            break
        fi
    done
fi
SDK="${SDK:-/c/ESP8266/sdk/ESP8266_RTOS_SDK}"
SDK_ROOT="${ESP_SDK_ROOT:-$(dirname "$SDK")}"
PROJ="${ESP_HELLO_PROJ:-$REPO_DIR/esp8266_rtos_clean/hello_uart}"
TOOLCHAIN_BIN="$SDK_ROOT/xtensa-lx106-elf/bin"
SETUP_SCRIPT="$SCRIPT_DIR/esp8266_rtos_msys_setup.sh"
PY_SHIMS="$SCRIPT_DIR/python_shims"
PY_VENV_BIN="$REPO_DIR/.local-tools/esp8266_py/bin"
export ESP_REPO_DIR="$REPO_DIR"

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

force_esp8285_1mb_dout_config()
{
    cat > sdkconfig.defaults <<'EOF'
CONFIG_ESPTOOLPY_FLASHMODE_DOUT=y
CONFIG_ESPTOOLPY_FLASHMODE="dout"
CONFIG_ESPTOOLPY_FLASHFREQ_40M=y
CONFIG_ESPTOOLPY_FLASHFREQ="40m"
CONFIG_ESPTOOLPY_FLASHSIZE_1MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="1MB"
CONFIG_ESPTOOLPY_FLASHSIZE_DETECT=n
EOF

    local marker=".config_esp8285_1mb_dout_v2"
    if [ ! -f "$marker" ] || ! grep -q 'CONFIG_ESPTOOLPY_FLASHMODE="dout"' sdkconfig 2>/dev/null || ! grep -q 'CONFIG_ESPTOOLPY_FLASHSIZE="1MB"' sdkconfig 2>/dev/null; then
        echo "Forcing ESP8285 flash config: DOUT, 40MHz, 1MB -> clean rebuild"
        rm -rf build sdkconfig
        make defconfig
        python - <<'PY'
from pathlib import Path
p = Path('sdkconfig')
s = p.read_text(encoding='utf-8') if p.exists() else ''
forced = {
    'CONFIG_ESPTOOLPY_FLASHMODE_DOUT': 'y',
    'CONFIG_ESPTOOLPY_FLASHMODE': '"dout"',
    'CONFIG_ESPTOOLPY_FLASHFREQ_40M': 'y',
    'CONFIG_ESPTOOLPY_FLASHFREQ': '"40m"',
    'CONFIG_ESPTOOLPY_FLASHSIZE_1MB': 'y',
    'CONFIG_ESPTOOLPY_FLASHSIZE': '"1MB"',
    'CONFIG_ESPTOOLPY_FLASHSIZE_DETECT': 'n',
}
lines = []
seen = set()
for line in s.splitlines():
    key = line.split('=', 1)[0] if '=' in line else None
    if key in forced:
        lines.append(f'{key}={forced[key]}')
        seen.add(key)
    else:
        lines.append(line)
for key, val in forced.items():
    if key not in seen:
        lines.append(f'{key}={val}')
p.write_text('\n'.join(lines) + '\n', encoding='utf-8')
print('sdkconfig forced: dout 40m 1MB')
PY
        touch "$marker"
    else
        echo "ESP8285 flash config already forced: DOUT, 40MHz, 1MB"
    fi
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
    rm -rf build sdkconfig .config_esp8285_1mb_dout_v2
fi

force_esp8285_1mb_dout_config
make -j"${NUMBER_OF_PROCESSORS:-4}"

echo
echo "OK: hello_uart build finished."
echo "Build directory: $PROJ/build"
