#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${IDF_PATH:?IDF_PATH is required}"
SDK_ROOT="${ESP_SDK_ROOT:?ESP_SDK_ROOT is required}"
PROJECT="${ESP_PROJECT:-$REPO_DIR/firmware}"
TOOLCHAIN_BIN="$SDK_ROOT/xtensa-lx106-elf/bin"
PY_VENV_BIN="$REPO_DIR/.local-tools/esp8266_py/bin"
PY_SHIMS="$REPO_DIR/tools/python_shims"
SETUP_SCRIPT="$REPO_DIR/tools/esp8266_rtos_msys_setup.sh"
MAKE_BIN="make"

test -f "$SDK/make/project.mk"
test -x "$TOOLCHAIN_BIN/xtensa-lx106-elf-gcc.exe"

cd "$PROJECT"
INPUT_HASH="$({
    find main -type f -print0 | sort -z | xargs -0 sha256sum
    sha256sum Makefile sdkconfig partitions_1mb_ota.csv 2>/dev/null || true
} | sha256sum | cut -d' ' -f1)"
STAMP="build/esp8285-sta-klink.inputs.sha256"
if [ "${ESP_CLEAN_BUILD:-0}" != 1 ] &&
   [ -f build/esp8285-sta-klink.ota.bin ] &&
   [ -f build/ota_data_initial.bin ] &&
   [ -f "$STAMP" ] &&
   [ "$(tr -d '\r\n' < "$STAMP")" = "$INPUT_HASH" ]; then
    echo "ESP_BUILD_UP_TO_DATE $PROJECT/build/esp8285-sta-klink.ota.bin"
    exit 0
fi

export ESP_REPO_DIR="$REPO_DIR"
bash "$SETUP_SCRIPT"
test -x "$PY_VENV_BIN/python"

export PATH="$PY_VENV_BIN:$TOOLCHAIN_BIN:$PATH"
export PYTHONPATH="$PY_SHIMS${PYTHONPATH:+:$PYTHONPATH}"
export IDF_PATH="$SDK"
export IDF_PYTHON_ENV_PATH=""

if [ "${ESP_RECONFIGURE:-0}" = 1 ]; then
    rm -f sdkconfig
fi
if [ ! -f sdkconfig ]; then
    make defconfig
fi

grep -q '^CONFIG_ESPTOOLPY_FLASHMODE="dout"$' sdkconfig
grep -q '^CONFIG_ESPTOOLPY_FLASHFREQ="40m"$' sdkconfig
grep -q '^CONFIG_ESPTOOLPY_FLASHSIZE="1MB"$' sdkconfig
grep -q '^CONFIG_ESP8266_XTAL_FREQ_40=y$' sdkconfig
grep -q '^CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200$' sdkconfig
grep -q '^CONFIG_FREERTOS_HZ=1000$' sdkconfig
grep -q '^CONFIG_ESP8266_HSPI_HIGH_THROUGHPUT=y$' sdkconfig
grep -q '^CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y$' sdkconfig
grep -q '^# CONFIG_ESP8266_WIFI_NVS_ENABLED is not set$' sdkconfig
grep -q '^CONFIG_PARTITION_TABLE_CUSTOM=y$' sdkconfig
grep -q '^CONFIG_PARTITION_TABLE_FILENAME="partitions_1mb_ota.csv"$' sdkconfig

if [ "${ESP_CLEAN_BUILD:-0}" = 1 ]; then
    make clean
fi
echo "ESP_BUILD_STAGE parallel-app"
rm -f build/esp8266/esp8266_out.ld
"$MAKE_BIN" -j"${NUMBER_OF_PROCESSORS:-4}" all_binaries 2>&1 |
    sed '/jobserver unavailable: using -j1/d;/сервер заданий недоступен: используется -j1/d'
echo "ESP_BUILD_STAGE dual-slot"
"$MAKE_BIN" -j1 ota 2>&1
"$MAKE_BIN" blank_ota_data

test -f "build/esp8285-sta-klink.ota.bin"
test -f "build/ota_data_initial.bin"
python - <<'PY'
from pathlib import Path
import struct

app1 = Path("build/esp8285-sta-klink.app1.bin").read_bytes()
app2 = Path("build/esp8285-sta-klink.app2.bin").read_bytes()
ota = Path("build/esp8285-sta-klink.ota.bin").read_bytes()
app1_load = struct.unpack_from("<I", app1, 8)[0]
app2_load = struct.unpack_from("<I", app2, 8)[0]
if (len(app1) != len(app2) or app1 == app2 or app1_load == app2_load or
        ota != app1 + app2):
    raise SystemExit("invalid OTA build: dual-slot package mismatch")
PY
printf '%s\n' "$INPUT_HASH" > "$STAMP"
echo "ESP_BUILD_OK $PROJECT/build/esp8285-sta-klink.ota.bin"
