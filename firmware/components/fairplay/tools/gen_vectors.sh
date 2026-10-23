#!/bin/sh
# Regenerates src/fp_vectors.c from the Go reference implementation.
#
# The reference package is not vendored: point FAIRPLAY_GO_DIR at the directory
# that holds the six source files (fairplay_sap.go, fairplay_md5.go,
# fairplay_message.go, fairplay_crypto.go, fpsap.go, fpsap_tables.go), or rely
# on the default location. The test file in gen_vectors/ is copied next to them
# in a scratch module so it can call the package internals.
set -e

here=$(cd "$(dirname "$0")" && pwd)
component=$(cd "$here/.." && pwd)
src=${FAIRPLAY_GO_DIR:-$HOME/airplay/doubletake/internal/airplay}
out=$component/src/fp_vectors.c
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for f in fairplay_sap.go fairplay_md5.go fairplay_message.go fairplay_crypto.go fpsap.go fpsap_tables.go; do
    if [ ! -f "$src/$f" ]; then
        echo "missing $src/$f (set FAIRPLAY_GO_DIR)" >&2
        exit 1
    fi
    cp "$src/$f" "$work/"
done
cp "$here/gen_vectors/gen_vectors_test.go" "$work/"
printf 'module fairplayvectors\n\ngo 1.22\n' >"$work/go.mod"

(cd "$work" && FP_VECTORS_OUT="$out" go test -run TestEmitVectors -count=1 ./...)

echo "wrote $out"
