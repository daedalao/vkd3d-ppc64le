#!/bin/bash
# HAND-MAINTAINED.  Builds and checks every configuration of the D3D12 thunk.
#
#   1. regenerate, and take the expected counts from the generator itself
#   2. guest half, -O0 and -O2                     -- compile + link + structure
#   3. the ms_abi forwarders really convert        -- disassembly
#   4. exactly the intended symbols are exported
#   5. host half, -O0 and -O2, with no vkd3d dependency
#   6. loopback (both halves in one process), -O0 and -O2  -- compile + run
#   7. the STRUCT_IFACE inventory, its warning, and its VKD3D_THUNK_STRICT abort
#   8. the MS-x64-caller test, -O0 and -O2         -- must PASS
#   9. the same test with the ms_abi forwarders disabled -- must FAIL
#
# Every configuration is built at -O0 AND at -O2.  -O2 is not optional: an
# earlier generator in the D3D11 project lost all its stubs at -O2 because they
# were `static` and provably unreachable, and the -O0 build gave no hint.
#
# Step 9 is the falsification of step 8.  A test that cannot fail proves
# nothing, and the calling-convention defect it covers survived two milestones
# in the D3D11 project precisely because no test had an MS-x64 caller.
#
# This machine is x86-64, which is BETTER than the D3D11 project's environment:
# both the SysV and the ms_abi call paths run natively, with no emulator in the
# way.  What it cannot do is compile the ppc64le host half; that is built on the
# POWER box with the same commands and PPC_MCPU set.
#
# Usage: tests/build.sh
set -u

cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
OUT=$ROOT/build
mkdir -p "$OUT"

CXX=${CXX:-g++}
# runtime/vkd3d_struct_fixups.cpp is the ONE runtime file that sees the d3d12
# types, so the widl headers (and vkd3d_windows.h, which they need first) are on
# the include path.  Nothing else in generated/ or runtime/ includes them, and
# the "exported symbols" and "boundary is three functions" checks below would
# notice if that changed.
INC="-I$ROOT/generated -I$ROOT/runtime -I$ROOT/../idl/gen -I$ROOT/../../include"
WARN="-Wall -Wextra -Wno-unused-parameter"
STD="-std=c++17 -fvisibility=hidden"
ARCH=$(uname -m)
MARCH=""
if [ "$ARCH" = "ppc64le" ]; then
  MARCH="-mcpu=${PPC_MCPU:-power8}"
fi

fail=0
step() { printf '\n=== %s\n' "$*"; }
check() { if [ "$1" -eq 0 ]; then echo "  ok"; else echo "  FAILED"; fail=1; fi; }

GUEST_SRC="generated/vkd3d_thunk_guest.cpp runtime/vkd3d_proxy.cpp runtime/vkd3d_struct_fixups.cpp"
HOST_SRC="generated/vkd3d_thunk_host.cpp runtime/vkd3d_thunk_host_rt.cpp"

# ---------------------------------------------------------------- generate
step "regenerate"
python3 ./gen_thunk.py --out generated/ > "$OUT/gen.log" 2>&1
check $?
# The struct-with-interface inventory is long and is the point of this package;
# keep the head of it in the log and the counts here.
head -28 "$OUT/gen.log" | sed 's/^/  /'
echo "  (full generator summary in $OUT/gen.log)"
# Expected counts come from the generator, so they cannot drift out of date.
. ./generated/vkd3d_thunk_counts.sh

# ------------------------------------------------------------ 1. guest half
for O in O0 O2; do
  step "guest half, $ARCH, -$O"
  $CXX $MARCH $STD $WARN -$O -shared -fPIC $INC \
      -o "$OUT/libvkd3d_thunk_guest_$O.so" $GUEST_SRC 2>&1 | sed 's/^/  /'
  check "${PIPESTATUS[0]}"
done

step "guest -O2: worker stubs, ms_abi forwarders and vtables survived -O2"
# Both are C++-mangled.  A forwarder is `<Iface>_<slot>_ms` and its first
# parameter is Proxy*, so `_<slot>_msP5Proxy` identifies it exactly; every other
# `_<slot>_<Name>` symbol is a SysV worker.
ALLST=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | grep -E ' [tT] _Z[0-9]+[A-Za-z0-9_]+_[0-9]+_')
WORK=$(printf '%s\n' "$ALLST" | grep -vcE '_[0-9]+_msP5Proxy')
MSFW=$(printf '%s\n' "$ALLST" | grep -cE '_[0-9]+_msP5Proxy')
VT_S=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | grep -cE ' [dDrR] k[A-Za-z0-9_]+_vtbl_sysv$')
VT_M=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | grep -cE ' [dDrR] k[A-Za-z0-9_]+_vtbl_ms$')
VT_T=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | grep -cE ' [dDrR] k[A-Za-z0-9_]+_target$')
echo "  worker stubs      $WORK (want $VKD3D_N_WORKERS)"
echo "  ms_abi forwarders $MSFW (want $VKD3D_N_FORWARDERS)"
echo "  vtbl_sysv/vtbl_ms/target  $VT_S/$VT_M/$VT_T (want $VKD3D_N_IFACES each)"
[ "$WORK" = "$VKD3D_N_WORKERS" ] && [ "$MSFW" = "$VKD3D_N_FORWARDERS" ] && \
[ "$VT_S" = "$VKD3D_N_IFACES" ] && [ "$VT_M" = "$VKD3D_N_IFACES" ] && \
[ "$VT_T" = "$VKD3D_N_IFACES" ]
check $?

step "guest -O2: the struct fixups are linked in, hidden, and wired"
# One extern symbol per fixup shape, all hidden: they are reached only through
# the generated vtables inside this library.  The shape symbols are
# vkd3d_fixup_<Method> with no underscore in the tail, which is what separates
# them from the four vkd3d_fixup_*_count diagnostic hooks.
# vkd3d_thunk_vtable()[slot] really pointing at them is what the loopback test
# proves; this proves the symbols survived -O2 at all.
FIXSYM=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | \
           grep -cE ' [tT] vkd3d_fixup_[A-Za-z]+$')
FIXTGT=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | \
           grep -cE ' [dDrR] kVkdFixupTargets$')
FIXEXP=$(nm -D --defined-only "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | \
           grep -cE ' T vkd3d_fixup_')
echo "  fixup shape symbols $FIXSYM (want $VKD3D_N_FIXUP_KINDS)"
echo "  kVkdFixupTargets $FIXTGT (want 1), exported fixups $FIXEXP (want 0)"
[ "$FIXSYM" = "$VKD3D_N_FIXUP_KINDS" ] && [ "$FIXTGT" = "1" ] && \
[ "$FIXEXP" = "0" ]
check $?

if [ "$ARCH" = "x86_64" ]; then
step "guest -O2: the ms_abi forwarders really convert the convention"
# Two shapes, both read off the generated prototypes rather than guessed:
#
#   ClearDepthStencilView(this, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
#                         D3D12_CLEAR_FLAGS flags, FLOAT depth, ...)
#     The by-value 8-byte handle is INTEGER-class, so it occupies an integer
#     position and `depth` is the FOURTH argument: MS-x64 puts it in XMM3, SysV
#     puts the first float in XMM0.  The forwarder must therefore contain
#     `movaps %xmm3,%xmm0`, plus `%rcx->%rdi` (this) and `%rdx->%rsi` (the
#     handle).  If the ms_abi attribute were missing, none of those moves would
#     be there -- the function would already be SysV.
#
#   OMSetDepthBounds(this, FLOAT min, FLOAT max)
#     Two floats: MS-x64 XMM1/XMM2, SysV XMM0/XMM1, i.e. a two-register shift.
DSVSLOT=$(grep -oP '(?<=^    )\d+(?=, /\* VKD3D_FSHAPE_CLEAR_DEPTH_STENCIL_VIEW)' \
            generated/vkd3d_thunk_ids.h)
BNDSLOT=$(grep -oP '(?<=^    )\d+(?=, /\* VKD3D_FSHAPE_OM_SET_DEPTH_BOUNDS)' \
            generated/vkd3d_thunk_ids.h)
SYM=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" | \
        grep -oE "_Z[0-9]+ID3D12GraphicsCommandList_${DSVSLOT}_ms[A-Za-z0-9]*" | head -1)
ASM=$(objdump -d --no-show-raw-insn "$OUT/libvkd3d_thunk_guest_O2.so" | \
        grep -A20 "<$SYM>:")
echo "  ClearDepthStencilView: slot $DSVSLOT symbol $SYM"
echo "$ASM" | grep -qE 'movaps +%xmm3,%xmm0' && \
echo "$ASM" | grep -qE 'mov +%rcx,%rdi' && \
echo "$ASM" | grep -qE 'mov +%rdx,%rsi'
check $?
echo "$ASM" | grep -E '%xmm3,%xmm0|%rcx,%rdi|%rdx,%rsi|%r8d,%edx' | sed 's/^/    /'

SYM2=$(nm "$OUT/libvkd3d_thunk_guest_O2.so" | \
        grep -oE "_Z[0-9]+ID3D12GraphicsCommandList1_${BNDSLOT}_ms[A-Za-z0-9]*" | head -1)
ASM2=$(objdump -d --no-show-raw-insn "$OUT/libvkd3d_thunk_guest_O2.so" | \
        grep -A12 "<$SYM2>:")
echo "  OMSetDepthBounds: slot $BNDSLOT symbol $SYM2"
echo "$ASM2" | grep -qE 'movaps +%xmm1,%xmm0' && \
echo "$ASM2" | grep -qE 'movaps +%xmm2,%xmm1'
check $?
echo "$ASM2" | grep -E '%xmm1,%xmm0|%xmm2,%xmm1|%rcx,%rdi' | sed 's/^/    /'
fi

step "guest -O2: the intended symbols are exported, and nothing else"
# Eight flat d3d12.dll entry points, three cross-runtime interop entry points
# (runtime/vkd3d_thunk_abi.h, "cross-runtime interop"), and the two ABI-mode
# calls the same header exports.  Everything else -- every stub, every vtable,
# the whole proxy runtime -- is hidden.
WANT_SYMS='D3D12CreateDevice|D3D12GetDebugInterface|D3D12GetInterface|D3D12CreateRootSignatureDeserializer|D3D12CreateVersionedRootSignatureDeserializer|D3D12SerializeRootSignature|D3D12SerializeVersionedRootSignature|D3D12EnableExperimentalFeatures|vkd3d_thunk_unwrap|vkd3d_thunk_wrap|vkd3d_thunk_interop_version|vkd3d_thunk_set_abi_sysv|vkd3d_thunk_abi_is_ms'
EXPORTED=$(nm -D --defined-only "$OUT/libvkd3d_thunk_guest_O2.so" 2>/dev/null | \
  grep -cE " T ($WANT_SYMS)\$")
LEAK=$(nm -D --defined-only "$OUT/libvkd3d_thunk_guest_O2.so" | grep -E ' [TDB] ' | \
  grep -vcE " ($WANT_SYMS)\$")
echo "  exported $EXPORTED (want 13), extra $LEAK (want 0)"
[ "$EXPORTED" = "13" ] && [ "$LEAK" = "0" ]
check $?

step "guest -O2: the boundary really is three functions"
# The whole point of the design (FEX's host trampoline allocator never frees):
# 2343 methods and 8 exports reach the host through exactly three symbols, and
# the guest half must not have picked up a direct call into the host half.
UNDEF=$(nm -D --undefined-only "$OUT/libvkd3d_thunk_guest_O2.so" | \
  grep -cE ' U (vkd3d|D3D12)')
NEEDED=$(nm -D --undefined-only "$OUT/libvkd3d_thunk_guest_O2.so" | \
  grep -cE ' U vkd3d_thunk_call(_float|_entry)?$')
echo "  undefined vkd3d/D3D12 symbols: $UNDEF (want 3), of which boundary calls: $NEEDED"
[ "$UNDEF" = "3" ] && [ "$NEEDED" = "3" ]
check $?

# ------------------------------------------------------------- 2. host half
for O in O0 O2; do
  step "host half, $ARCH${MARCH:+ $MARCH}, -$O"
  $CXX $MARCH $STD $WARN -$O -shared -fPIC $INC \
      -o "$OUT/libvkd3d_thunk_host_$O.so" $HOST_SRC -ldl -lpthread 2>&1 | sed 's/^/  /'
  check "${PIPESTATUS[0]}"
done

step "host: no build-time dependency on vkd3d"
readelf -d "$OUT/libvkd3d_thunk_host_O2.so" | grep -qi vkd3d
[ $? -ne 0 ]
check $?

# ------------------------------------------------------------ 3. loopback
for O in O0 O2; do
  step "loopback test, $ARCH, -$O"
  $CXX $MARCH $STD $WARN -$O $INC -o "$OUT/loopback_$O" \
      tests/loopback.cpp $GUEST_SRC $HOST_SRC -ldl -lpthread 2>&1 | sed 's/^/  /'
  check "${PIPESTATUS[0]}"
done

for O in O0 O2; do
  step "run loopback -$O"
  stdbuf -o0 "$OUT/loopback_$O" > "$OUT/loopback_$O.log" 2>&1
  rc=$?
  sed 's/^/  /' "$OUT/loopback_$O.log"
  [ $rc -eq 0 ]
  check $?
done

# ------------------------------- 4. the struct-with-interface residue
step "STRUCT_IFACE slots warn on every call"
WARNS=$(grep -c 'STRUCT-IFACE' "$OUT/loopback_O2.log")
echo "  STRUCT-IFACE warning lines in the run: $WARNS (want at least 1)"
[ "$WARNS" -ge 1 ]
check $?

step "VKD3D_THUNK_STRICT=1 turns a STRUCT_IFACE call into an abort"
"$OUT/loopback_O2" --struct-abort > "$OUT/struct_ok.log" 2>&1
OKRC=$?
VKD3D_THUNK_STRICT=1 "$OUT/loopback_O2" --struct-abort > "$OUT/struct_strict.log" 2>&1
STRICTRC=$?
echo "  without STRICT: exit $OKRC (want 0)"
echo "  with STRICT:    exit $STRICTRC (want non-zero)"
grep -h 'aborting because' "$OUT/struct_strict.log" | sed 's/^/    /'
[ "$OKRC" -eq 0 ] && [ "$STRICTRC" -ne 0 ] && grep -q 'aborting because' "$OUT/struct_strict.log"
check $?

step "the untranslated-slot inventory is exactly the documented residue"
FIXED=$VKD3D_N_STRUCTIFACE_FIXED LEFT=$VKD3D_N_STRUCTIFACE_LEFT python3 - <<'EOF'
import os, re, sys
h = open('generated/vkd3d_thunk_ids.h').read()
# Same predicate as vkd3d_slot_untranslated() in the generated header.
IFACE_PTRS = 1 | 2 | 4 | 8
HANDLED    = 16 | 32
STRUCT     = 256
REFUSED    = 128
FIXUP      = 4096
REPORTED   = REFUSED | STRUCT
names = re.findall(r'VKD3D_IFACE_(\w+) = \d+,', h)
bad, tot, kinds, fixed = [], 0, {}, 0
for m in re.finditer(r'kVkdSlotFlags_(\w+)\[\] = \{([^}]*)\}', h):
    for slot, f in enumerate(int(x) for x in m.group(2).split(',')):
        tot += 1
        if f & FIXUP:
            fixed += 1
        if ((f & IFACE_PTRS) and not (f & HANDLED)) or \
           ((f & STRUCT) and not (f & FIXUP)) or (f & REFUSED):
            bad.append((m.group(1), slot, f))
            k = 'struct-iface' if (f & STRUCT) else \
                ('refused' if (f & REFUSED) else 'other')
            kinds[k] = kinds.get(k, 0) + 1
print("  %d of %d slots would be reported by vkd3d_slot_untranslated()" % (len(bad), tot))
for k in sorted(kinds):
    print("    %-14s %d" % (k, kinds[k]))
print("    %-14s %d (struct fixups, no longer reported)" % ('fixed', fixed))
# Every one of them must be a struct-with-interface slot or a refused slot;
# nothing may be reported for any other reason, and nothing carrying an
# interface pointer may be unhandled.  And the residue plus the fixed slots
# must be the whole struct-with-interface inventory.
ok = all((f & REPORTED) for _, _, f in bad)
want_fixed = int(os.environ['FIXED'])
want_left  = int(os.environ['LEFT'])
if fixed != want_fixed:
    print("    MISMATCH: %d fixed slots, the generator says %d" % (fixed, want_fixed))
if kinds.get('struct-iface', 0) != want_left:
    print("    MISMATCH: %d struct slots left, the generator says %d"
          % (kinds.get('struct-iface', 0), want_left))
ok = ok and fixed == want_fixed and kinds.get('struct-iface', 0) == want_left
sys.exit(0 if ok and kinds.get('other', 0) == 0 else 1)
EOF
check $?

# ---------------------------------------------- 5. the MS-x64 caller
if [ "$ARCH" = "x86_64" ]; then
  for O in O0 O2; do
    step "MS-x64-caller test, x86-64, -$O"
    $CXX $STD $WARN -$O $INC -o "$OUT/msabi_$O" \
        tests/msabi_caller.cpp $GUEST_SRC $HOST_SRC -ldl -lpthread 2>&1 | sed 's/^/  /'
    check "${PIPESTATUS[0]}"

    step "run MS-x64-caller test -$O"
    "$OUT/msabi_$O" > "$OUT/msabi_$O.log" 2>&1
    rc=$?
    sed 's/^/  /' "$OUT/msabi_$O.log"
    [ $rc -eq 0 ]
    check $?
  done

  # ------------------- 6. falsification: the same test, the defect put back
  step "negative control: rebuild with the ms_abi forwarders disabled"
  $CXX $STD $WARN -O2 $INC -DVKD3D_THUNK_ABI_NEGATIVE_CONTROL \
      -o "$OUT/msabi_negative" \
      tests/msabi_caller.cpp $GUEST_SRC $HOST_SRC -ldl -lpthread 2>&1 | sed 's/^/  /'
  check "${PIPESTATUS[0]}"

  step "negative control MUST fail (this is the calling-convention defect)"
  "$OUT/msabi_negative" > "$OUT/negative.log" 2>&1
  NRC=$?
  # FAILED-vs-FAIL discipline: this log is SUPPOSED to contain FAIL lines, so
  # they are counted, not searched for as a build error.
  NFAIL=$(grep -c 'FAIL ' "$OUT/negative.log")
  echo "  exit status $NRC (want non-zero)"
  echo "  FAIL lines   $NFAIL (want at least 1: the seam check reports before"
  echo "               the first dereference kills the process)"
  head -6 "$OUT/negative.log" | sed 's/^/    /'
  [ "$NRC" -ne 0 ] && [ "$NFAIL" -ge 1 ]
  check $?
else
  step "not x86-64: the MS-x64 caller and its negative control DID NOT RUN"
  echo "  ms_abi does not exist on $ARCH; run this script on the x86-64 box too"
  fail=1
fi

step "summary"
echo "  interfaces $VKD3D_N_IFACES, slots $VKD3D_N_SLOTS, workers $VKD3D_N_WORKERS,"
echo "  forwarders $VKD3D_N_FORWARDERS, hand-written $VKD3D_N_HAND,"
echo "  marshalled $VKD3D_N_MARSHALLED, aggregate returns $VKD3D_N_AGGRET,"
echo "  by-value aggregates $VKD3D_N_BYVALAGG, float slots $VKD3D_N_FLOATSLOTS"
echo "  in $VKD3D_N_FLOATSHAPES shapes, struct-iface $VKD3D_N_STRUCTIFACE"
echo "  ($VKD3D_N_STRUCTIFACE_FIXED fixed by runtime/vkd3d_struct_fixups.cpp in"
echo "  $VKD3D_N_FIXUP_KINDS shapes, $VKD3D_N_STRUCTIFACE_LEFT left + STRICT-flagged),"
echo "  refused $VKD3D_N_REFUSED, pump overrides $VKD3D_N_PUMP"
grep -h 'passed,' "$OUT"/loopback_O2.log "$OUT"/msabi_O2.log 2>/dev/null | sed 's/^/  /'

printf '\n=== %s\n' "$([ $fail -eq 0 ] && echo ALL OK || echo FAILURES)"
exit $fail
