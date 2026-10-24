#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$ROOT/Sources/CaptureCLI/main.swift"
OUT="${1:-$ROOT/capture-cli}"
SDK=$(xcrun --sdk macosx --show-sdk-path)

COMMON_FLAGS="-parse-as-library -O -target arm64-apple-macos12.3 -sdk $SDK"

# Keep the default build small and fast. Pass --universal as the second argument
# when preparing a binary for both Apple Silicon and Intel Macs.
if [ "${2:-}" = "--universal" ]; then
    TMP=$(mktemp -d "${TMPDIR:-/tmp}/capture-cli.XXXXXX")
    trap 'rm -rf "$TMP"' EXIT INT TERM
    swiftc $COMMON_FLAGS -o "$TMP/capture-cli-arm64" "$SRC" \
        -framework ScreenCaptureKit -framework VideoToolbox -framework CoreMedia \
        -framework CoreVideo -framework CoreGraphics -framework AVFoundation
    swiftc -parse-as-library -O -target x86_64-apple-macos12.3 -sdk "$SDK" \
        -o "$TMP/capture-cli-x86_64" "$SRC" \
        -framework ScreenCaptureKit -framework VideoToolbox -framework CoreMedia \
        -framework CoreVideo -framework CoreGraphics -framework AVFoundation
    lipo -create "$TMP/capture-cli-arm64" "$TMP/capture-cli-x86_64" -output "$OUT"
else
    swiftc $COMMON_FLAGS -o "$OUT" "$SRC" \
        -framework ScreenCaptureKit -framework VideoToolbox -framework CoreMedia \
        -framework CoreVideo -framework CoreGraphics -framework AVFoundation
fi

printf 'built %s\n' "$OUT"
