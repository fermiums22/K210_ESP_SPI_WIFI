#!/usr/bin/env bash
set -euo pipefail

# One-shot MSYS2 dependency setup for official ESP8266_RTOS_SDK v3.4.
# Only install package names that exist in the MSYS repo used by C:\msys64\usr\bin\bash.exe.
# The ESP8266 xtensa toolchain is handled separately by the Windows BAT file.

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

if ! command -v pacman >/dev/null 2>&1; then
    echo "ERROR: pacman not found. This script must run inside MSYS2."
    exit 2
fi

echo "Installing required MSYS2 packages for ESP8266_RTOS_SDK v3.4 build..."
pacman -S --needed --noconfirm "${MSYS_PACKAGES[@]}"
hash -r

echo "Checking Python build-time core modules..."
python - <<'PY'
import pyexpat
import pkg_resources
print('Python build dependency smoke test OK')
PY

echo "MSYS2 package setup OK."
