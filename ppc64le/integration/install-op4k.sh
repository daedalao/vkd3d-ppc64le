#!/bin/bash
# Install the vkd3d-ppc64le thunk stack on op4k. Idempotent. Run ON op4k.
#
# Makes a PRIVATE hardlink copy of stock GE-Proton and swaps two files into
# it; never modifies the source Proton or any other agent's directories.
# cp -al means shared files are hardlinks: replacements are done rm-first so
# the originals are never written through a link.
#
#   PROTON_SRC   stock Proton to clone      (~/proton/GE-Proton11-3)
#   PROTON_DEST  our private variant        (~/proton-ge-vkd3dthunk)
#   BUILD        pe-shim build dir          (repo ppc64le/pe-shim/build)
#   NATIVE       native vkd3d build         (repo build-native)
#   HOSTTHUNKS   FEX host thunk dir         (~/.local/share/fex-emu/HostThunks)
#   LIBDIR       host-side lib dir with the .so.0 dlopen-gate symlink
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$(dirname "$HERE")")"

PROTON_SRC="${PROTON_SRC:-$HOME/proton/GE-Proton11-3}"
PROTON_DEST="${PROTON_DEST:-$HOME/proton-ge-vkd3dthunk}"
BUILD="${BUILD:-$REPO/ppc64le/pe-shim/build}"
NATIVE="${NATIVE:-$REPO/build-native}"
HOSTTHUNKS="${HOSTTHUNKS:-$HOME/.local/share/fex-emu/HostThunks}"
LIBDIR="${LIBDIR:-$HOME/vkd3d-native-libs}"

for f in "$BUILD/d3d12.dll" "$BUILD/libvkd3d_d3d12-guest.so" \
         "$BUILD/libvkd3d_d3d12-host.so" \
         "$NATIVE/libs/d3d12/libvkd3d-proton-d3d12.so" \
         "$NATIVE/libs/d3d12core/libvkd3d-proton-d3d12core.so"; do
    [ -f "$f" ] || { echo "missing: $f (build first)"; exit 1; }
done

echo "== private Proton variant: $PROTON_DEST"
if [ ! -d "$PROTON_DEST" ]; then
    cp -al "$PROTON_SRC" "$PROTON_DEST"
    echo "   cloned (hardlinks) from $PROTON_SRC"
fi

swap() { # swap DEST-relative path with SRC file, rm-first (hardlink safety)
    local rel="$1" src="$2"
    rm -f "$PROTON_DEST/$rel"
    cp "$src" "$PROTON_DEST/$rel"
    echo "   swapped $rel  <- $(basename "$src")"
}

# The PE shim replaces vkd3d-proton's x86-64 d3d12.dll; the guest ELF thunk
# becomes its wine-unixlib counterpart, which is how the shim's route-1000
# (builtin) loader finds it: wine derives <root>/x86_64-unix/d3d12.so from a
# builtin loaded out of <root>/x86_64-windows/d3d12.dll.
#
# Wine decides a PE is one of its own builtins PURELY by the 17 bytes at file
# offset 0x40 reading "Wine builtin DLL\0" (dxvk-ppc64le measured this the
# hard way; their deployed d3d11.dll is stamped the same). Without the stamp,
# override=b finds no builtin and LoadLibrary fails with error 126. A stamped
# image can no longer be loaded as a native dll -- fine, this copy is only
# ever a builtin.
swap files/lib/wine/x86_64-windows/d3d12.dll "$BUILD/d3d12.dll"
printf 'Wine builtin DLL\0' | dd of="$PROTON_DEST/files/lib/wine/x86_64-windows/d3d12.dll" \
    bs=1 seek=64 conv=notrunc status=none
echo "   stamped as Wine builtin"
rm -f "$PROTON_DEST/files/lib/wine/x86_64-unix/d3d12.so"
cp "$BUILD/libvkd3d_d3d12-guest.so" "$PROTON_DEST/files/lib/wine/x86_64-unix/d3d12.so"
echo "   installed files/lib/wine/x86_64-unix/d3d12.so (guest ELF thunk)"
# default_pfx copy so fresh prefixes carry the shim as their system32 d3d12
swap files/share/default_pfx/drive_c/windows/system32/d3d12.dll "$BUILD/d3d12.dll"

echo "== FEX host thunk: $HOSTTHUNKS"
install -Dm755 "$BUILD/libvkd3d_d3d12-host.so" "$HOSTTHUNKS/libvkd3d_d3d12-host.so"

echo "== native libs + dlopen gate: $LIBDIR"
mkdir -p "$LIBDIR"
ln -sf "$NATIVE/libs/d3d12/libvkd3d-proton-d3d12.so" "$LIBDIR/libvkd3d-proton-d3d12.so"
ln -sf "$NATIVE/libs/d3d12core/libvkd3d-proton-d3d12core.so" "$LIBDIR/libvkd3d-proton-d3d12core.so"
# thunkgen's generated loader dlopens this exact name before anything else
ln -sf "$NATIVE/libs/d3d12/libvkd3d-proton-d3d12.so" "$LIBDIR/libvkd3d_d3d12.so.0"

echo "== done. Env every wine/Proton launch needs:"
echo "   LD_LIBRARY_PATH=$LIBDIR"
echo "   (host thunk resolves from $HOSTTHUNKS via FEX config)"
