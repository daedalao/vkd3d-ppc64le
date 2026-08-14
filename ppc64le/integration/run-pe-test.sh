#!/bin/bash
# The PE hop: wine (x86-64, emulated under FEX) loads d3d12.dll (PE shim),
# which reaches the guest ELF thunk through wine's unixlib mechanism, which
# crosses 0F 3F into native ppc64le vkd3d-proton. Run ON op4k after
# install-op4k.sh.
#
# Uses a private prefix and Proton's wine directly -- no Steam, no game.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$(dirname "$HERE")")"

PROTON_DEST="${PROTON_DEST:-$HOME/proton-ge-vkd3dthunk}"
BUILD="${BUILD:-$REPO/ppc64le/pe-shim/build}"
LIBDIR="${LIBDIR:-$HOME/vkd3d-native-libs}"
PREFIX="${PREFIX:-$HOME/vkd3d-pe-prefix}"

WINE="$PROTON_DEST/files/bin/wine64"
[ -x "$WINE" ] || WINE="$PROTON_DEST/files/bin/wine"

export WINEPREFIX="$PREFIX"
# builtin: load OUR d3d12.dll from the proton build dir (which route-1000
# unixlib resolution keys off), never a prefix-local native copy.
export WINEDLLOVERRIDES="d3d12=b;mscoree=;mshtml="
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VKD3D_FILTER_DEVICE_NAME="${VKD3D_FILTER_DEVICE_NAME:-V620}"
export WINEDEBUG="${WINEDEBUG:--all}"

mkdir -p "$PREFIX"
if [ ! -f "$PREFIX/system.reg" ]; then
    echo "== initializing prefix (first run)"
    "$WINE" wineboot --init >/dev/null 2>&1 || true
    "$PROTON_DEST/files/bin/wineserver" -w 2>/dev/null || true
fi

echo "== running pe_attach.exe under $WINE"
exec "$WINE" "$BUILD/pe_attach.exe"
