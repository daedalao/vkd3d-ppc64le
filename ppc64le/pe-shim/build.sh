#!/bin/bash
# Build the three halves of the vkd3d-proton D3D12 thunk, out of tree.
#
# Out of tree on purpose: the FEX checkout is read-only to this work. Every
# command below is the same one FEX's own CMake would run for a thunked library
# -- the shape is lifted from dxvk-ppc64le/pe-shim/build.sh, which lifted it
# from build-thunks/build.ninja's rules for libxshmfence -- with the paths
# pointed at this directory. The in-tree equivalent is fex-registration.patch.
#
# Produces build/:
#   d3d12.dll                  x86-64 PE shim                 [any x86-64 box]
#   pe_attach.exe              x86-64 PE attach test          [any x86-64 box]
#   libvkd3d_d3d12-guest.so    x86-64 ELF, FEX guest thunk + wine unixlib
#   libvkd3d_d3d12-host.so     ppc64le ELF, FEX host thunk    [POWER box only]
#   attach                     x86-64 ELF attach test
#
# THIS SCRIPT IS DESIGNED TO RUN IN A PARTIALLY-BUILT WORLD. Three things it
# needs may legitimately be absent, and each one degrades to a named TODO
# rather than to a failure:
#
#   * FEX's thunkgen binary. Without it the two .inl files cannot be generated,
#     so instead the C++ halves are SYNTAX-CHECKED against hand-written stub
#     .inls that declare exactly what the generator emits. That catches
#     everything except a signature disagreement with thunkgen itself.
#   * ppc64le/thunk/{generated,runtime}/*.cpp -- another agent's work. Without
#     them the guest .so has no COM runtime to link and the host .so has no
#     dispatcher, so only the compile of THIS directory's sources happens.
#   * a ppc64le host. The host half is never cross-compiled here; on any other
#     machine the exact commands are printed for the POWER box to run.
#
# Env: FEX, FEXBUILD, ROOTFS, XTOOLS, PPC_MCPU, VKD3D_NATIVE.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PPC="$(dirname "$HERE")"          # ppc64le/
# FEX tree default: probe the layouts of the two boxes this actually runs on
# (the x86-64 dev box keeps fex-ppc64le as a sibling of this repo; op4k keeps
# the live fork at ~/projects/fex-emu-ppc64le/src), then the AC922's path from
# the reference docs. FEX= always overrides.
if [ -z "${FEX:-}" ]; then
    for c in "$(dirname "$(dirname "$PPC")")/fex-ppc64le" \
             "$HOME/projects/fex-emu-ppc64le/src" \
             "$HOME/Development/fastppcx86"; do
        if [ -d "$c/ThunkLibs/include" ]; then FEX="$c"; break; fi
    done
    FEX="${FEX:-$HOME/Development/fastppcx86}"
fi
FEXBUILD="${FEXBUILD:-$FEX/build-thunks}"
ROOTFS="${ROOTFS:-$HOME/Development/fexrootfs/RootFS/Ubuntu_24_04}"
XTOOLS="${XTOOLS:-$HOME/Development/fexrootfs/x-tools/x86_64-linux-gnu}"
# Where a native ppc64le vkd3d-proton build lives, for the dlopen-gate symlink.
VKD3D_NATIVE="${VKD3D_NATIVE:-$(dirname "$PPC")/build-native/libs/d3d12}"

THUNKGEN="$FEXBUILD/Bin/thunkgen"
GEN="$PPC/thunk/generated"
RT="$PPC/thunk/runtime"
OUT="$HERE/build"
ARCH="$(uname -m)"
mkdir -p "$OUT/gen_guest" "$OUT/gen_host" "$OUT/synstub_guest" "$OUT/synstub_host"

RC=0
TODO=()
step() { printf '\n=== %s\n' "$*"; }
note() { printf '  %s\n' "$*"; }
todo() { TODO+=("$1"); printf '  TODO(%s): %s\n' "${2:-POWER box}" "$1"; }
fail() { printf '  FAILED: %s\n' "$*"; RC=1; }
must() { if "$@"; then note "ok"; else fail "$*"; fi; }

# --------------------------------------------------------------------------
step "toolchain"
CLANG="${CLANG:-clang}"
CLANGXX="${CLANGXX:-clang++}"
command -v "$CLANG" >/dev/null || { echo "no clang; nothing here can build"; exit 1; }
note "$($CLANG --version | head -1)"

# The PE import libraries need a dlltool. llvm-dlltool is preferred (it is part
# of the same LLVM that compiles the PE); mingw's is a drop-in for this use --
# an import library is only a table of names.
DLLTOOL=""
for t in llvm-dlltool x86_64-w64-mingw32-dlltool; do
  if command -v "$t" >/dev/null; then DLLTOOL="$t"; break; fi
done
if [ -n "$DLLTOOL" ]; then
  note "dlltool: $DLLTOOL"
else
  note "dlltool: NONE -- the PE half will be skipped"
fi

# --------------------------------------------------------------------------
step "thunkgen: guest + host"
HAVE_INL=0
if [ -x "$THUNKGEN" ]; then
  note "using $THUNKGEN"
  PPC_CXX_INC=$(ls -d /usr/include/c++/*/powerpc64le-*linux-gnu 2>/dev/null | head -1)
  "$THUNKGEN" \
    "$HERE/libvkd3d_d3d12_interface.cpp" libvkd3d_d3d12 -guest \
    "$OUT/gen_guest/thunkgen_guest_libvkd3d_d3d12.inl" "$ROOTFS" \
    -- -std=c++20 \
    --target=x86_64-linux-gnu \
    --sysroot="$ROOTFS" \
    --gcc-toolchain="$XTOOLS" \
    -isystem"$FEX/ThunkLibs/include" \
    -isystem"$FEX/FEXCore/include" \
    -isystem"$HERE/include" \
  && "$THUNKGEN" \
    "$HERE/libvkd3d_d3d12_interface.cpp" libvkd3d_d3d12 -host \
    "$OUT/gen_host/thunkgen_host_libvkd3d_d3d12.inl" "$ROOTFS" \
    -- -std=c++20 \
    -DTHUNK_HOST_NOT_X86_64 -DARCHITECTURE_ppc64le=1 \
    -isystem"$FEX/ThunkLibs/include" \
    -isystem"$FEX/FEXCore/include" \
    -isystem"$HERE/include" \
    ${PPC_CXX_INC:+-isystem"$PPC_CXX_INC"} \
  && HAVE_INL=1 || fail "thunkgen"
  if [ "$HAVE_INL" = 1 ]; then
    note "generated host export table:"
    grep -o 'libvkd3d_d3d12:[a-z0-9_]*' "$OUT/gen_host/thunkgen_host_libvkd3d_d3d12.inl" | sed 's/^/    /'
  fi
else
  todo "run thunkgen: $THUNKGEN not present (set FEXBUILD to a FEX build with Bin/thunkgen)" "any box with a FEX build"
fi

# Stub .inls, for the syntax probe below. They declare EXACTLY what
# ThunkLibs/Generator/gen.cpp emits for four custom_host_impl functions:
# fexfn_pack_* on the guest side (gen.cpp, packing functions) and the
# fexfn_impl_* forward declarations plus exports[]/fexldr_init_* on the host
# side (gen.cpp:765, :984). They are never linked into anything.
cat > "$OUT/synstub_guest/thunkgen_guest_libvkd3d_d3d12.inl" <<'EOF'
/* SYNTAX PROBE ONLY -- generated by build.sh, never linked.
 * Mirrors what thunkgen emits for the four custom_host_impl functions. The
 * extern "C" is not decoration: gen.cpp wraps the whole fexfn_pack_ block in
 * one (gen.cpp:612), so without it the probe would resolve C++-mangled names
 * and the link probe below would report the wrong symbols. */
extern "C" {
uint64_t fexfn_pack_vkd3d_host_dispatch(uint32_t, uint32_t, uint64_t, uint64_t);
uint64_t fexfn_pack_vkd3d_host_dispatch_float(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint32_t fexfn_pack_vkd3d_host_entry(uint32_t, uint64_t);
uint32_t fexfn_pack_vkd3d_host_probe();
}
EOF
cat > "$OUT/synstub_host/thunkgen_host_libvkd3d_d3d12.inl" <<'EOF'
/* SYNTAX PROBE ONLY -- generated by build.sh, never linked. */
static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_dispatch(uint32_t, uint32_t, uint64_t, uint64_t) -> uint64_t;
static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_dispatch_float(uint32_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) -> uint64_t;
static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_entry(uint32_t, uint64_t) -> uint32_t;
static auto fexfn_impl_libvkd3d_d3d12_vkd3d_host_probe() -> uint32_t;
static ExportEntry exports[] = {{nullptr, nullptr}};
extern "C" bool fexldr_init_libvkd3d_d3d12() { return true; }
EOF

# --------------------------------------------------------------------------
step "guest ELF library (x86-64)"
# On a ppc64le build host the guest half is a cross-compile into the FEX
# rootfs; on x86-64 the native toolchain already targets it.
GUEST_FLAGS=(
  -std=gnu++20 -O3 -DNDEBUG -fPIC -fwrapv -msse2 -mfpmath=sse
  -fvisibility=hidden
  -DGUEST_THUNK_LIBRARY
  -I"$HERE/include"
  -I"$GEN"
  -I"$RT"
  # runtime/vkd3d_struct_fixups.cpp is the one guest TU that sees the d3d12
  # types (the struct-with-interface fixups have to know what a
  # D3D12_RESOURCE_BARRIER is); everything else stays header-free.
  -I"$PPC/idl/gen"
  -I"$(dirname "$PPC")/include"
  -I"$FEX/ThunkLibs/include"
  -I"$FEX/FEXCore/include"
)
if [ "$ARCH" != "x86_64" ]; then
  GUEST_FLAGS=(
    --target=x86_64-linux-gnu
    --sysroot="$ROOTFS"
    --gcc-toolchain="$XTOOLS"
    -fuse-ld=lld
    "${GUEST_FLAGS[@]}"
    -idirafter /usr/include
  )
fi

# The COM runtime and the generated vtable stubs are ppc64le/thunk's, compiled
# in unmodified. Only guest_thunk.o is this directory's. Names follow
# dxvk-ppc64le's thunk/ layout; adjust here when ppc64le/thunk lands.
GUEST_RT_SRC=()
for f in "$GEN/vkd3d_thunk_guest.cpp" "$RT/vkd3d_proxy.cpp" \
         "$RT/vkd3d_struct_fixups.cpp"; do
  [ -f "$f" ] && GUEST_RT_SRC+=("$f")
done
GUEST_RT_WANT=3

if [ "$HAVE_INL" = 1 ] && [ "${#GUEST_RT_SRC[@]}" = "$GUEST_RT_WANT" ]; then
  OBJS=()
  $CLANGXX "${GUEST_FLAGS[@]}" -I"$OUT/gen_guest" -c "$HERE/libvkd3d_d3d12_Guest.cpp" -o "$OUT/guest_thunk.o" || fail "guest thunk TU"
  OBJS+=("$OUT/guest_thunk.o")
  for f in "${GUEST_RT_SRC[@]}"; do
    o="$OUT/guest_$(basename "${f%.cpp}").o"
    $CLANGXX "${GUEST_FLAGS[@]}" -I"$OUT/gen_guest" -c "$f" -o "$o" || fail "compile $f"
    OBJS+=("$o")
  done
  $CLANGXX "${GUEST_FLAGS[@]}" -shared -Wl,-soname,libvkd3d_d3d12-guest.so \
    -o "$OUT/libvkd3d_d3d12-guest.so" "${OBJS[@]}" || fail "link guest .so"
  if [ -f "$OUT/libvkd3d_d3d12-guest.so" ]; then
    note "exported boundary + entry points:"
    nm -D --defined-only "$OUT/libvkd3d_d3d12-guest.so" \
      | grep -E ' [TDB] (vkd3d_thunk_|D3D12|__wine_unix_call_funcs)' | sed 's/^/    /'
  fi
elif [ "${#GUEST_RT_SRC[@]}" = "$GUEST_RT_WANT" ]; then
  # No thunkgen, but ppc64le/thunk IS here. That allows a much stronger check
  # than a syntax probe: compile every guest TU for real and try to link them,
  # with the stub .inl standing in. The link MUST fail, and it must fail on
  # EXACTLY the four thunkgen packers -- anything else in the missing set means
  # this directory and ppc64le/thunk disagree about a symbol, which is the one
  # class of defect that would otherwise wait for the POWER box to find.
  note "no thunkgen; running the LINK PROBE instead (compile everything, expect exactly 4 unresolved)"
  todo "link libvkd3d_d3d12-guest.so for real once thunkgen is available" "any box with a FEX build"
  mkdir -p "$OUT/linkprobe"
  # -O1 rather than -O3: this is a symbol-completeness probe, and the generated
  # vtable TU is 1.7 MB of source.
  PROBE_FLAGS=("${GUEST_FLAGS[@]/-O3/-O1}")
  POBJS=()
  $CLANGXX "${PROBE_FLAGS[@]}" -Wall -I"$OUT/synstub_guest" -c "$HERE/libvkd3d_d3d12_Guest.cpp" -o "$OUT/linkprobe/guest_thunk.o" \
    || fail "guest thunk TU"
  POBJS+=("$OUT/linkprobe/guest_thunk.o")
  for f in "${GUEST_RT_SRC[@]}"; do
    o="$OUT/linkprobe/$(basename "${f%.cpp}").o"
    $CLANGXX "${PROBE_FLAGS[@]}" -c "$f" -o "$o" || fail "compile $f"
    POBJS+=("$o")
  done
  $CLANGXX "${PROBE_FLAGS[@]}" -shared -Wl,--no-undefined -o "$OUT/linkprobe/probe.so" "${POBJS[@]}" \
    > "$OUT/linkprobe/link.log" 2>&1
  MISSING=$(grep -oE 'fexfn_pack_[A-Za-z0-9_]+|undefined reference to `[^'"'"']+' "$OUT/linkprobe/link.log" \
            | sed "s/undefined reference to \`//" | sed 's/(.*//' | sort -u)
  printf '%s\n' "$MISSING" | sed 's/^/    /'
  EXPECT=$'fexfn_pack_vkd3d_host_dispatch\nfexfn_pack_vkd3d_host_dispatch_float\nfexfn_pack_vkd3d_host_entry\nfexfn_pack_vkd3d_host_probe'
  if [ -f "$OUT/linkprobe/probe.so" ]; then
    fail "the link SUCCEEDED -- the stub .inl is being linked in; the probe proves nothing"
    rm -f "$OUT/linkprobe/probe.so"
  elif [ "$MISSING" = "$EXPECT" ]; then
    note "ok: the only unresolved symbols are the four thunkgen packers"
  else
    fail "unexpected unresolved symbols (see $OUT/linkprobe/link.log)"
  fi
else
  note "ppc64le/thunk/{generated,runtime} sources not present (have ${#GUEST_RT_SRC[@]}/$GUEST_RT_WANT)"
  todo "link libvkd3d_d3d12-guest.so once thunkgen and ppc64le/thunk are available" "any box"
  note "syntax probe instead (stub .inl, real FEX headers):"
  must $CLANGXX "${GUEST_FLAGS[@]}" -Wall -I"$OUT/synstub_guest" -fsyntax-only "$HERE/libvkd3d_d3d12_Guest.cpp"
fi

# --------------------------------------------------------------------------
step "host ppc64le library"
HOST_FLAGS=(
  "-mcpu=${PPC_MCPU:-power8}"
  -std=gnu++20 -O3 -DNDEBUG -fPIC -fwrapv -Wall
  -DARCHITECTURE_ppc64le=1 -DTHUNK_HOST_NOT_X86_64
  -I"$OUT/gen_host"
  -I"$HERE/include"
  -I"$GEN"
  -I"$RT"
  -I"$FEX/ThunkLibs/include"
  -I"$FEX/FEXCore/include"
  -I"$FEX/Source"
  -I"$FEXBUILD/Source"
)
HOST_RT_SRC=()
for f in "$GEN/vkd3d_thunk_host.cpp" "$RT/vkd3d_thunk_host_rt.cpp"; do
  [ -f "$f" ] && HOST_RT_SRC+=("$f")
done

if [ "$ARCH" = "ppc64le" ] && [ "$HAVE_INL" = 1 ] && [ "${#HOST_RT_SRC[@]}" = 2 ]; then
  $CLANGXX "${HOST_FLAGS[@]}" -c "$HERE/libvkd3d_d3d12_Host.cpp" -o "$OUT/host_thunk.o" || fail "host thunk TU"
  for f in "${HOST_RT_SRC[@]}"; do
    $CLANGXX "${HOST_FLAGS[@]}" -c "$f" -o "$OUT/host_$(basename "${f%.cpp}").o" || fail "compile $f"
  done
  # --no-undefined is what turns a missing custom_host_impl, or a name the host
  # runtime spells differently, into a link error instead of a null call at run
  # time. Keep it.
  $CLANGXX -shared -Xlinker --no-undefined \
    -Wl,-soname,libvkd3d_d3d12-host.so \
    -o "$OUT/libvkd3d_d3d12-host.so" \
    "$OUT/host_thunk.o" "$OUT"/host_vkd3d_thunk_host*.o -ldl || fail "link host .so"
  [ -f "$OUT/libvkd3d_d3d12-host.so" ] && nm -D --defined-only "$OUT/libvkd3d_d3d12-host.so" | grep fexthunks | sed 's/^/    /'
else
  if [ "$ARCH" != "ppc64le" ]; then
    note "this box is $ARCH; the host half is NEVER cross-compiled here. Run on the POWER box:"
    printf '    clang++ %s -c %s -o build/host_thunk.o\n' "${HOST_FLAGS[*]}" "$HERE/libvkd3d_d3d12_Host.cpp"
    printf '    clang++ %s -c <thunk/generated/vkd3d_thunk_host.cpp> -o build/host_dispatch.o\n' "${HOST_FLAGS[*]}"
    printf '    clang++ %s -c <thunk/runtime/vkd3d_thunk_host_rt.cpp> -o build/host_rt.o\n' "${HOST_FLAGS[*]}"
    printf '    clang++ -shared -Xlinker --no-undefined -Wl,-soname,libvkd3d_d3d12-host.so \\\n'
    printf '            -o build/libvkd3d_d3d12-host.so build/host_thunk.o build/host_dispatch.o build/host_rt.o -ldl\n'
  fi
  todo "build libvkd3d_d3d12-host.so (needs a ppc64le box, thunkgen, and ppc64le/thunk host sources)"

  # The host sources are architecture-independent C++ -- -DARCHITECTURE_ppc64le
  # only selects declarations -- so they can be compiled and LINKED anywhere as
  # a contract probe. That is worth much more than a syntax check: it is what
  # proves libvkd3d_d3d12_Host.cpp's four asm-labelled declarations really
  # resolve to ppc64le/thunk/runtime's vkd3d_host_dispatch / _dispatch_float /
  # _entry / _probe, which is the single most likely thing to be wrong across
  # the two directories.
  #
  # The .so it produces is NOT a build artifact -- wrong arch, wrong -mcpu, stub
  # .inl -- which is why it goes in linkprobe/ and is named probe.
  PROBE_HOST_FLAGS=(-std=gnu++20 -O1 -fPIC -fwrapv -Wall -Wno-unused-function
    -DARCHITECTURE_ppc64le=1 -DTHUNK_HOST_NOT_X86_64
    -I"$OUT/synstub_host" -I"$HERE/include" -I"$GEN" -I"$RT"
    -I"$FEX/ThunkLibs/include" -I"$FEX/FEXCore/include")
  if [ "${#HOST_RT_SRC[@]}" = 2 ]; then
    note "host LINK PROBE (native arch, stub .inl -- proves the symbol contract, not a build artifact):"
    mkdir -p "$OUT/linkprobe"
    HOBJS=()
    $CLANGXX "${PROBE_HOST_FLAGS[@]}" -c "$HERE/libvkd3d_d3d12_Host.cpp" -o "$OUT/linkprobe/host_thunk.o" || fail "host thunk TU"
    HOBJS+=("$OUT/linkprobe/host_thunk.o")
    for f in "${HOST_RT_SRC[@]}"; do
      o="$OUT/linkprobe/$(basename "${f%.cpp}").o"
      $CLANGXX "${PROBE_HOST_FLAGS[@]}" -c "$f" -o "$o" || fail "compile $f"
      HOBJS+=("$o")
    done
    if $CLANGXX -shared -Wl,--no-undefined -o "$OUT/linkprobe/host_probe.so" "${HOBJS[@]}" -ldl > "$OUT/linkprobe/hostlink.log" 2>&1; then
      note "ok: links with --no-undefined; the runtime provides all four host entry points"
      nm -D --defined-only "$OUT/linkprobe/host_probe.so" | grep -E ' T vkd3d_host_' | sed 's/^/    /'
    else
      fail "host link probe (see $OUT/linkprobe/hostlink.log)"
      grep -oE "undefined reference to .[^'\`]+" "$OUT/linkprobe/hostlink.log" | sed "s/.*to .//" | sort -u | head | sed 's/^/    /'
    fi
  else
    note "ppc64le/thunk host sources not present; syntax probe only:"
    must $CLANGXX "${PROBE_HOST_FLAGS[@]}" -fsyntax-only "$HERE/libvkd3d_d3d12_Host.cpp"
  fi
fi

# --------------------------------------------------------------------------
step "the thunkgen dlopen gate"
# libvkd3d_d3d12_interface.cpp explains this in full: thunkgen's generated
# loader dlopens "<thunk name>.so.<version>", which for us is
# libvkd3d_d3d12.so.0, and vkd3d-proton's native library is called something
# else entirely. The name is provided as a symlink.
if [ -f "$VKD3D_NATIVE/libvkd3d-proton-d3d12.so" ]; then
  ln -sf "$VKD3D_NATIVE/libvkd3d-proton-d3d12.so" "$OUT/libvkd3d_d3d12.so.0"
  note "build/libvkd3d_d3d12.so.0 -> $VKD3D_NATIVE/libvkd3d-proton-d3d12.so"
else
  todo "ln -sf <native>/libvkd3d-proton-d3d12.so \$FEX_THUNKHOSTLIBS/libvkd3d_d3d12.so.0 -- without it the host thunk fails to load with 'Failed to initialize thunk library' (set VKD3D_NATIVE=)"
fi

# --------------------------------------------------------------------------
step "PE shim (x86-64 COFF)"
# No mingw-w64 sysroot is assumed: no CRT, no startup files, no system import
# libraries. The one import library needed is synthesised from pe/ntdll.def.
#
# -nostdinc is deliberately NOT used: it removes clang's own resource-directory
# headers along with the system ones, and <stdint.h> lives there. For a Windows
# target clang adds no Linux /usr/include anyway.
if [ -n "$DLLTOOL" ]; then
  PE_CFLAGS=(--target=x86_64-windows-gnu -O2 -std=c11 -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -Wall -Wextra)
  PE_LDFLAGS=(--target=x86_64-windows-gnu -nostdlib -fuse-ld=lld)

  "$DLLTOOL" -m i386:x86-64 -d "$HERE/pe/ntdll.def" -l "$OUT/libntdll.a" || fail "dlltool ntdll"
  $CLANG "${PE_CFLAGS[@]}" -I"$HERE/include" -c "$HERE/pe/d3d12_shim.c" -o "$OUT/d3d12_shim.o" || fail "compile d3d12_shim.c"
  $CLANG "${PE_LDFLAGS[@]}" -shared -Wl,--entry,DllMainCRTStartup -Wl,--kill-at \
    -o "$OUT/d3d12.dll" "$OUT/d3d12_shim.o" "$OUT/libntdll.a" || fail "link d3d12.dll"

  # Import library, in case anything wants to link against the shim rather than
  # LoadLibrary it.
  "$DLLTOOL" -m i386:x86-64 -d "$HERE/pe/d3d12.def" -l "$OUT/libd3d12.a" || fail "dlltool d3d12"

  # --- verification, not decoration ---------------------------------------
  # An export that silently went missing (a typo in a __declspec, a linker that
  # dropped a data symbol) would show up as GetProcAddress returning NULL in a
  # game, months later. Assert the list here.
  if [ -f "$OUT/d3d12.dll" ]; then
    file "$OUT/d3d12.dll" | sed 's/^/    /'
    EXPORTED=$(llvm-readobj --coff-exports "$OUT/d3d12.dll" 2>/dev/null | sed -n 's/^ *Name: //p' | sort)
    WANT=$(grep -oE '^    D3D12[A-Za-z]+' "$HERE/pe/d3d12.def" | tr -d ' ' | sort)
    MISSING=$(comm -13 <(printf '%s\n' "$EXPORTED") <(printf '%s\n' "$WANT"))
    printf '%s\n' "$EXPORTED" | sed 's/^/    /'
    if [ -n "$MISSING" ]; then
      fail "d3d12.dll is missing exports: $(printf '%s' "$MISSING" | tr '\n' ' ')"
    else
      note "all $(printf '%s\n' "$WANT" | wc -l) declared exports present"
    fi
    note "imports:"
    llvm-readobj --coff-imports "$OUT/d3d12.dll" 2>/dev/null | grep -E 'Name:|Symbol:' | sed 's/^/    /'
  fi

  # --- the PE attach test --------------------------------------------------
  "$DLLTOOL" -m i386:x86-64 -d "$HERE/pe/kernel32.def" -l "$OUT/libkernel32.a" || fail "dlltool kernel32"
  $CLANG "${PE_CFLAGS[@]}" -c "$HERE/tests/pe_attach.c" -o "$OUT/pe_attach.o" || fail "compile pe_attach.c"
  $CLANG "${PE_LDFLAGS[@]}" -Wl,--entry,mainCRTStartup -Wl,--subsystem,console \
    -o "$OUT/pe_attach.exe" "$OUT/pe_attach.o" "$OUT/libkernel32.a" || fail "link pe_attach.exe"
  [ -f "$OUT/pe_attach.exe" ] && file "$OUT/pe_attach.exe" | sed 's/^/    /'
else
  todo "build d3d12.dll: no dlltool (install llvm or mingw-w64-binutils)" "this box"
fi

# --------------------------------------------------------------------------
step "attach test program (x86-64 guest ELF)"
ATTACH_FLAGS=(-O2 -Wall -I"$HERE/include")
if [ "$ARCH" != "x86_64" ]; then
  ATTACH_FLAGS=(--target=x86_64-linux-gnu --sysroot="$ROOTFS" --gcc-toolchain="$XTOOLS" -fuse-ld=lld "${ATTACH_FLAGS[@]}")
fi
must $CLANG "${ATTACH_FLAGS[@]}" -o "$OUT/attach" "$HERE/tests/attach.c" -ldl

# --------------------------------------------------------------------------
step "gpu round-trip test program (x86-64 guest ELF)"
must $CLANG "${ATTACH_FLAGS[@]}" -o "$OUT/gpu_roundtrip" "$HERE/tests/gpu_roundtrip.c" -ldl

# --------------------------------------------------------------------------
step "summary"
ls -l "$OUT" 2>/dev/null | grep -E 'd3d12\.dll|pe_attach\.exe|-guest\.so|-host\.so|attach$|so\.0' | sed 's/^/    /'
if [ "${#TODO[@]}" != 0 ]; then
  printf '\n  %d thing(s) deferred:\n' "${#TODO[@]}"
  for t in "${TODO[@]}"; do printf '    - %s\n' "$t"; done
fi
[ "$RC" = 0 ] && printf '\n  build: OK\n' || printf '\n  build: FAILURES\n'
exit "$RC"
