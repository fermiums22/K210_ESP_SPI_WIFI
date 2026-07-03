#!/usr/bin/env bash
set -euo pipefail

# One-shot MSYS2 dependency setup for official ESP8266_RTOS_SDK v3.4.
# Only install package names that exist in the MSYS repo used by C:\msys64\usr\bin\bash.exe.
# The ESP8266 xtensa toolchain is handled separately by the Windows BAT file.

LOCAL_TOOLS="/d/w_space/K210_ESP_SPI_WIFI/.local-tools"
PY_VENV="$LOCAL_TOOLS/esp8266_py"
SETUP_MARKER="$LOCAL_TOOLS/msys_setup_venv_pyparsing_ok"

MSYS_PACKAGES=(
    bash
    git
    make
    diffutils
    patch
    tar
    gzip
    unzip
    wget
    curl
    python
    python-setuptools
    python-pip
    python-packaging
    cmake
    ninja
    flex
    bison
    gperf
    gettext-devel
    ncurses-devel
    libexpat
)

venv_python()
{
    echo "$PY_VENV/bin/python"
}

ensure_python_venv()
{
    if [ ! -x "$(venv_python)" ]; then
        echo "Creating local Python venv: $PY_VENV"
        rm -rf "$PY_VENV"
        mkdir -p "$LOCAL_TOOLS"
        python -m venv "$PY_VENV"
    fi

    echo "Installing Python build module pyparsing==2.4.7 in local venv..."
    "$(venv_python)" -m pip install --disable-pip-version-check "pyparsing==2.4.7"
}

check_python_core()
{
    echo "Checking Python build-time modules..."
    "$(venv_python)" - <<'PY'
import pyexpat
import pyparsing
print('Python venv build dependency smoke test OK')
PY
}

if ! command -v pacman >/dev/null 2>&1; then
    echo "ERROR: pacman not found. This script must run inside MSYS2."
    exit 2
fi

if [ -f "$SETUP_MARKER" ]; then
    echo "MSYS2 package setup already checked."
    check_python_core
    echo "MSYS2 package setup OK."
    exit 0
fi

echo "Installing required MSYS2 packages for ESP8266_RTOS_SDK v3.4 build..."
pacman -S --needed --noconfirm "${MSYS_PACKAGES[@]}"
hash -r

ensure_python_venv
check_python_core
mkdir -p "$(dirname "$SETUP_MARKER")"
touch "$SETUP_MARKER"

echo "MSYS2 package setup OK."
