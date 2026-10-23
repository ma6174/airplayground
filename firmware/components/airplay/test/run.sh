#!/bin/sh
# Host test suite for the airplay component. No ESP-IDF required: the protocol
# modules are compiled against a thin POSIX platform layer and a plaintext
# stand-in for the HAP channel.
set -eu

cd "$(dirname "$0")"

CC=${CC:-clang}
CFLAGS="-std=c99 -Wall -Wextra -Werror -I../include -I../src -I../../hap/include -DAP_HOST_BUILD"
FAIRPLAY_INCLUDES="-I../../fairplay/include -I../../fairplay/src"

mkdir -p build out

python3 samples.py out

$CC $CFLAGS -o build/test_plist test_plist.c ../src/ap_plist.c ../src/ap_setup.c
$CC $CFLAGS -o build/test_frame test_frame.c ../src/ap_frame.c
$CC $CFLAGS -o build/test_client test_client.c ../src/ap_client.c ../src/ap_md5.c \
    ../src/ap_platform.c hap_stub.c
$CC $CFLAGS $FAIRPLAY_INCLUDES -isysroot $(xcrun --show-sdk-path 2>/dev/null) -o build/test_legacy \
    test_legacy.c ../src/ap_legacy.c ../src/ap_plist.c ../src/ap_frame.c ../src/ap_client.c \
    ../src/ap_md5.c ../src/ap_platform.c hap_stub.c legacy_host.c \
    ../../fairplay/src/fpsap.c ../../fairplay/src/fpsap_tables.c ../../fairplay/src/fp_md5.c \
    ../../fairplay/src/fp_sap.c ../../fairplay/src/fp_message.c ../../fairplay/test/fp_platform_host.c

./build/test_plist out
./build/test_frame
./build/test_client
./build/test_legacy
python3 check.py out
