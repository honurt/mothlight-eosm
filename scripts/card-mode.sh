#!/usr/bin/env bash
# Choose how a Mothlight card starts the camera.
#
#   scripts/card-mode.sh <card> canon       start as plain Canon; hold SET at power-on for Mothlight
#   scripts/card-mode.sh <card> mothlight   start as Mothlight; hold SET at power-on for plain Canon
#   scripts/card-mode.sh <card> status
#
# <card> is where the SD card is mounted, e.g. /Volumes/EOS_DIGITAL (macOS) or /media/$USER/EOS_DIGITAL.
# It only creates or removes ML/SETTINGS/REQUIRE.SET (Magic Lantern's own "SET at startup" setting).
set -euo pipefail

card="${1:-}"; mode="${2:-status}"
[ -n "$card" ] || { echo "usage: $0 <card mount point> canon|mothlight|status"; exit 1; }
[ -f "$card/autoexec.bin" ] && [ -d "$card/ML" ] || { echo "$card does not look like a Mothlight card (no autoexec.bin / ML)"; exit 1; }

flag="$card/ML/SETTINGS/REQUIRE.SET"
case "$mode" in
    canon)     mkdir -p "$card/ML/SETTINGS"; : > "$flag" ;;
    mothlight) rm -f "$flag" ;;
    status)    ;;
    *) echo "unknown mode '$mode' (canon | mothlight | status)"; exit 1 ;;
esac
sync

if [ -f "$flag" ]; then
    echo "Canon first: the camera starts stock; hold SET while turning it on for Mothlight."
else
    echo "Mothlight first: the camera starts Mothlight; hold SET while turning it on for plain Canon."
fi
