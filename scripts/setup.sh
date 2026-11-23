#!/usr/bin/env bash
# Install what the build needs on Ubuntu / Debian / WSL2.
# macOS: use the Dockerfile instead (docker build -t mothlight . && docker run --rm -v "$PWD":/src mothlight).
set -euo pipefail
source "$(dirname "$0")/lib.sh"

PKGS="git make gcc python3 python3-docutils python3-pil zip unzip gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi"

if command -v apt-get > /dev/null; then
    say "installing: $PKGS"
    SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
    $SUDO apt-get update -q
    # shellcheck disable=SC2086
    $SUDO apt-get install -y -q $PKGS
else
    warn "no apt-get: install these yourself: $PKGS"
fi

arm-none-eabi-gcc --version | head -1 || die "arm-none-eabi-gcc missing"
python3 -c "import docutils" || die "python3-docutils missing"
say "toolchain ready. next: scripts/fetch.sh && scripts/build.sh"
