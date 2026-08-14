#!/bin/sh
# widl stand-in for hosts without wine: serves the pregenerated headers in
# gen/. Wired in as the 'widl' binary via the meson native file that
# build-native.sh writes. Invoked by meson exactly as widl would be:
#
#   widl-shim.sh -h -o OUTPUT INPUT.idl
#
# Refuses to serve a header whose source .idl no longer matches the manifest
# recorded by gen.sh — a drifted .idl must be regenerated on a widl box, not
# silently served stale.
set -eu
here=$(cd "$(dirname "$0")" && pwd)

out= in=
while [ $# -gt 0 ]; do
    case $1 in
        -h) ;;
        -o) out=$2; shift ;;
        -*) echo "widl-shim: unhandled widl flag '$1'" >&2; exit 2 ;;
        *)  in=$1 ;;
    esac
    shift
done
[ -n "$out" ] && [ -n "$in" ] || {
    echo "widl-shim: expected '-h -o OUTPUT INPUT', got neither" >&2
    exit 2
}

base=$(basename "$in" .idl)
want=$(awk -v f="$base.idl" '$2 == f { print $1 }' "$here/manifest.sha256")
[ -n "$want" ] || {
    echo "widl-shim: $base.idl not in manifest — rerun ppc64le/idl/gen.sh on a widl box" >&2
    exit 2
}
got=$(sha256sum "$in" | cut -d' ' -f1)
[ "$got" = "$want" ] || {
    echo "widl-shim: $base.idl changed since headers were pregenerated" >&2
    echo "widl-shim: rerun ppc64le/idl/gen.sh on a widl box and commit the result" >&2
    exit 2
}
cp "$here/gen/$base.h" "$out"
