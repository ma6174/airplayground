#!/bin/sh
# Re-exports the Go implementation's constants as C initializers.
#
# Writes tools/fairplay_data.h, the input of tools/gen_tables.py. Only needed
# when the Go side changes: the generated component data files are checked in.
#
# Point FAIRPLAY_GO_DIR at the directory holding the six reference sources
# (fairplay_sap.go, fairplay_md5.go, fairplay_message.go, fairplay_crypto.go,
# fpsap.go, fpsap_tables.go) or rely on the default location.
set -e

here=$(cd "$(dirname "$0")" && pwd)
src=${FAIRPLAY_GO_DIR:-$HOME/airplay/doubletake/internal/airplay}
out=$here/fairplay_data.h
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

for f in fairplay_sap.go fairplay_md5.go fairplay_message.go fairplay_crypto.go fpsap.go fpsap_tables.go; do
    if [ ! -f "$src/$f" ]; then
        echo "missing $src/$f (set FAIRPLAY_GO_DIR)" >&2
        exit 1
    fi
    cp "$src/$f" "$work/"
done
cp "$here/export_data/emit_test.go" "$work/"
printf 'module fairplaydata\n\ngo 1.22\n' >"$work/go.mod"

(cd "$work" && EMIT_OUT="$out" go test -run TestEmitCConstants -count=1 ./...)

echo "wrote $out"
