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

test -f "$SDK/make/project.mk"
test -x "$TOOLCHAIN_BIN/xtensa-lx106-elf-gcc.exe"

export ESP_REPO_DIR="$REPO_DIR"
bash "$SETUP_SCRIPT"
test -x "$PY_VENV_BIN/python"

export PATH="$PY_VENV_BIN:$TOOLCHAIN_BIN:$PATH"
export PYTHONPATH="$PY_SHIMS${PYTHONPATH:+:$PYTHONPATH}"
export IDF_PATH="$SDK"
export IDF_PYTHON_ENV_PATH=""

cd "$PROJECT"
if [ ! -f sdkconfig ]; then
    make defconfig
fi

grep -q '^CONFIG_ESPTOOLPY_FLASHMODE="dout"$' sdkconfig
grep -q '^CONFIG_ESPTOOLPY_FLASHFREQ="40m"$' sdkconfig
grep -q '^CONFIG_ESPTOOLPY_FLASHSIZE="1MB"$' sdkconfig
grep -q '^CONFIG_ESP8266_XTAL_FREQ_40=y$' sdkconfig
grep -q '^CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200$' sdkconfig
grep -q '^CONFIG_ESP8266_HSPI_HIGH_THROUGHPUT=y$' sdkconfig
grep -q '^CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y$' sdkconfig
grep -q '^# CONFIG_ESP8266_WIFI_NVS_ENABLED is not set$' sdkconfig
grep -q '^CONFIG_PARTITION_TABLE_CUSTOM=y$' sdkconfig
grep -q '^CONFIG_PARTITION_TABLE_FILENAME="partitions_1mb_ota.csv"$' sdkconfig

make clean
make ota -j"${NUMBER_OF_PROCESSORS:-4}"
make blank_ota_data

test -f "build/esp8285-sta-klink.ota.bin"
test -f "build/ota_data_initial.bin"
python - <<'PY'
from pathlib import Path

app1 = Path("build/esp8285-sta-klink.app1.bin").read_bytes()
app2 = Path("build/esp8285-sta-klink.app2.bin").read_bytes()
if len(app1) != len(app2) or app1[:8] == app2[:8]:
    raise SystemExit("invalid OTA build: app1/app2 link images are not distinct")
PY
echo "ESP_BUILD_OK $PROJECT/build/esp8285-sta-klink.ota.bin"
