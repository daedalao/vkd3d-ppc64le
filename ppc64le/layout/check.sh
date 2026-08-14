#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build and run the D3D12 struct-layout probe on this machine, then -- if the
# other architecture's output is already sitting next to it -- prove the two
# agree byte for byte.
#
# The point: an emulated x86-64 game builds D3D12 structs to the SysV ABI and
# hands them to a native ppc64le vkd3d-proton built to ELFv2.  Nothing in the
# emulator rewrites those structs, so every size, alignment and member offset
# must already match.  This script is the proof.
#
#   ./ppc64le/layout/check.sh            build + run + compare if possible
#   ./ppc64le/layout/check.sh --regen    regenerate probe.c first (x86 side only)
#
# IMPORTANT: both machines must compile the SAME probe.c.  Regenerating it on
# only one side would compare two different questions, so --regen is opt-in and
# the generated probe.c is meant to be committed.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)

CC=${CC:-gcc}
regen=0
if [ "${1:-}" = "--regen" ]; then
    regen=1
fi

if [ "$regen" = 1 ]; then
    echo "regenerating probe.c ..."
    python3 "$here/gen_layout_probe.py"
fi

if [ ! -f "$here/probe.c" ]; then
    echo "check.sh: $here/probe.c is missing; run: python3 $here/gen_layout_probe.py" >&2
    exit 2
fi

arch=$(uname -m)
case "$arch" in
    x86_64|amd64) out=x86_64.txt; other=ppc64le.txt; otherarch=ppc64le ;;
    ppc64le|ppc64el) out=ppc64le.txt; other=x86_64.txt; otherarch=x86-64 ;;
    *)
        echo "check.sh: unexpected host architecture '$arch'; writing $arch.txt" >&2
        out="$arch.txt"; other=""; otherarch="" ;;
esac

echo "building probe for $arch with $CC ..."
"$CC" -std=c11 -I "$root/ppc64le/idl/gen" -I "$root/include" \
    "$here/probe.c" -o "$here/probe.$arch"

"$here/probe.$arch" > "$here/$out"
rm -f "$here/probe.$arch"

records=$(grep -c '^RECORD ' "$here/$out" || true)
members=$(grep -c '^MEMBER ' "$here/$out" || true)
enums=$(grep -c '^ENUM ' "$here/$out" || true)
bitfields=$(grep -c '^BITFIELD ' "$here/$out" || true)
echo "wrote $out: $records records, $members members, $enums enums, $bitfields bitfields"

if [ -z "$other" ] || [ ! -f "$here/$other" ]; then
    echo
    echo "No $other yet, so there is nothing to compare against."
    echo "Produce it on the $otherarch box, from a checkout of this same tree"
    echo "(same probe.c!), at the repository root:"
    echo
    echo "    gcc -std=c11 -I ppc64le/idl/gen -I include ppc64le/layout/probe.c -o probe"
    echo "    ./probe > ppc64le/layout/$other"
    echo
    echo "or, with probe.c copied into the current directory:"
    echo
    echo "    gcc -std=c11 -I ppc64le/idl/gen -I include probe.c -o probe && ./probe > $other"
    echo
    echo "then drop $other into ppc64le/layout/ and re-run this script."
    exit 0
fi

echo "comparing $out against $other ..."

# Both files come from the same probe.c, so the line order is identical; still,
# key the comparison on the record/member name so a missing line is reported as
# such instead of shifting everything after it.
awk -v a="$out" -v b="$other" '
    /^[ \t]*$/ { next }
    NR == FNR { key = $1 " " $2; A[key] = $0; order[++n] = key; next }
    { key = $1 " " $2; B[key] = $0 }
    END {
        diffs = 0
        for (i = 1; i <= n; i++) {
            k = order[i]
            if (!(k in B)) {
                printf "MISSING in %s: %s\n", b, A[k]
                diffs++
            } else if (A[k] != B[k]) {
                printf "%s: %s\n", a, A[k]
                printf "%s: %s\n", b, B[k]
                diffs++
            }
            delete B[k]
        }
        for (k in B) {
            printf "MISSING in %s: %s\n", a, B[k]
            diffs++
        }
        exit diffs == 0 ? 0 : 1
    }
' "$here/$out" "$here/$other" && rc=0 || rc=1

if [ "$rc" = 0 ]; then
    echo "LAYOUT PARITY: $records records, $members members, all identical"
    exit 0
fi

echo
echo "LAYOUT MISMATCH: the D3D12 ABI boundary is NOT layout-compatible."
exit 1
