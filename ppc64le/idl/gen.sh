#!/bin/sh
# Regenerate the pregenerated IDL headers used to build on hosts without
# widl (Arch POWER has no wine, hence no widl). Run this on a box that has
# widl (any box with wine installed), then commit gen/ and manifest.sha256.
#
# The manifest pins the .idl inputs these headers were generated from;
# widl-shim.sh refuses to serve a header whose .idl has drifted, so a stale
# pregen is a loud build error on the POWER box, never a silent mismatch.
set -eu
cd "$(dirname "$0")"
repo=$(cd ../.. && pwd)

command -v widl >/dev/null 2>&1 || {
    echo "gen.sh: no widl on PATH — run this on a box with wine installed" >&2
    exit 2
}

# The list mirrors vkd3d_idl in include/meson.build. vkd3d_unknown.idl is
# deliberately absent there too: it is import-only and does not compile
# standalone (widl errors on its bare REFIID typedef).
idl_names="
vkd3d_d3d12
vkd3d_d3d12sdklayers
vkd3d_d3dcommon
vkd3d_dxcapi
vkd3d_dxgi
vkd3d_dxgi1_2
vkd3d_dxgi1_3
vkd3d_dxgi1_4
vkd3d_dxgi1_5
vkd3d_dxgibase
vkd3d_dxgiformat
vkd3d_dxgitype
vkd3d_swapchain_factory
vkd3d_command_list_vkd3d_ext
vkd3d_command_queue_vkd3d_ext
vkd3d_device_vkd3d_ext
vkd3d_core_interface
"

mkdir -p gen
: > manifest.sha256
count=0
for base in $idl_names; do
    idl=$repo/include/$base.idl
    widl -h -o "gen/$base.h" "$idl"
    printf '%s  %s.idl\n' "$(sha256sum "$idl" | cut -d' ' -f1)" "$base" >> manifest.sha256
    count=$((count + 1))
done
echo "gen.sh: generated $count headers from include/*.idl"
