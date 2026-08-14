#!/bin/bash
# The exact invocation that loads the thunk pair and crosses it.
#
# Usage: ./run-attach.sh [d]        "d" additionally calls D3D12GetDebugInterface
#
# POWER BOX ONLY -- it runs an x86-64 guest ELF under FEX against a native
# ppc64le host thunk. Nothing here works on an x86-64 development box.
#
# Three things here are not obvious and cost time to find. The first two are
# dxvk-ppc64le's, learned the hard way and carried over verbatim:
#
#  * HOME is redirected. With ThunksDB enabled in ~/.config/fex-emu/Config.json
#    but no matching guest library present, FEX dies with SIGTRAP and no
#    message unless logging is on. A clean HOME sidesteps the live config
#    entirely rather than depending on it.
#
#  * FEX_ROOTFS must then be set EXPLICITLY, because the rootfs is otherwise
#    found through ~/.local/share/fex-emu. Without it FEX says "Invalid or
#    Unsupported elf file ... RootFS path set to ''", which reads like a
#    problem with the program rather than with the configuration.
#
#  * VKD3D_THUNK_ABI=sysv. The generated vtables default to MS-x64 on x86-64
#    because the deployment caller is a PE game (vkd3d_thunk_abi.h, "ABI
#    mode"). tests/attach.c is an ELF caller and must opt back in, or every
#    method call on a returned proxy jumps with its arguments in the wrong
#    registers -- silently, because every value involved is a plausible
#    integer. The PE path must NOT set this.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPC="$(dirname "$HERE")"
REPO="$(dirname "$PPC")"
B="$HERE/build"
N="${VKD3D_NATIVE:-$REPO/build-native/libs}"
FEXBIN="${FEXBIN:-$HOME/Development/fastppcx86/build/Bin/FEX}"
ROOTFS="${ROOTFS:-$HOME/Development/fexrootfs/RootFS/Ubuntu_24_04}"

export HOME=/tmp/vkd3dthunk-home
mkdir -p "$HOME"
export FEX_ROOTFS="$ROOTFS"

# FEX resolves the host thunk as "$FEX_THUNKHOSTLIBS/<name>-host.so"
# (Source/Tools/LinuxEmulation/Thunks.cpp).
export FEX_THUNKHOSTLIBS="$B"
export FEX_THUNKGUESTLIBS="$B"

# Native vkd3d-proton, dynamically attached. build/ is on the path because it
# is where build.sh puts the libvkd3d_d3d12.so.0 symlink that thunkgen's
# generated loader dlopens -- see libvkd3d_d3d12_interface.cpp. Without it the
# host thunk fails to load and FEX says so before any D3D12 call happens.
export LD_LIBRARY_PATH="$B:$N/d3d12:$N/d3d12core:$N/vkd3d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VKD3D_THUNK_D3D12_LIB="${VKD3D_THUNK_D3D12_LIB:-$N/d3d12/libvkd3d-proton-d3d12.so}"
export VKD3D_THUNK_TRACE=1

export VKD3D_THUNK_ABI=sysv

# vkd3d-proton's own knobs, so a failure is about the thunk rather than about
# device selection or a chatty log.
export VKD3D_DEBUG="${VKD3D_DEBUG:-warn}"

# Device selection for the gpu round trip: the box has a V620 and two V340s;
# the V620 is the deployment target.
export VKD3D_FILTER_DEVICE_NAME="${VKD3D_FILTER_DEVICE_NAME:-V620}"

if [ "${1:-}" = "gpu" ]; then
    exec "$FEXBIN" "$B/gpu_roundtrip" "$B/libvkd3d_d3d12-guest.so"
fi
exec "$FEXBIN" "$B/attach" "$B/libvkd3d_d3d12-guest.so" "${1:-}"
