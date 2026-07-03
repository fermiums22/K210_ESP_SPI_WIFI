#!/usr/bin/env bash
set -euo pipefail

# One-shot MSYS2 dependency setup for official ESP8266_RTOS_SDK v3.4.
# Keep this list explicit and boring so the Windows bring-up is reproducible.

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
    python-wheel
    python-packaging
    python-pyserial
    python-click
    python-cryptography
    python-pyparsing
    python-pyelftools
    python-future
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

echo "Installing required MSYS2 packages for ESP8266_RTOS_SDK v3.4..."
pacman -S --needed --noconfirm "${MSYS_PACKAGES[@]}"
hash -r

echo "Checking Python core modules..."
python - <<'PY'
import pyexpat
import pkg_resources
import serial
import click
import cryptography
import pyparsing
import elftools
import future
print('Python dependency smoke test OK')
PY

echo "MSYS2 package setup OK."
