#!/bin/sh
# Host test suite for the media component. No ESP-IDF required: the bitstream
# helpers and the pattern generator are plain C. The esp_h264 encoder itself
# only runs on the chip and is covered by the on-device logs.
set -eu

cd "$(dirname "$0")"

CC=${CC:-clang}
CFLAGS="-std=c99 -Wall -Wextra -Werror -I../include"

mkdir -p build

$CC $CFLAGS -o build/test_media test_media.c ../src/media_avcc.c ../src/media_pattern.c ../src/media_text.c

./build/test_media
