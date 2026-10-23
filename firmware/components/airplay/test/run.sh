#!/bin/sh
# Host test suite for the airplay component. No ESP-IDF required: the protocol
# modules are compiled against a thin POSIX platform layer and a plaintext
# stand-in for the HAP channel.
set -eu

cd "$(dirname "$0")"

CC=${CC:-clang}
CFLAGS="-std=c99 -Wall -Wextra -Werror -I../include -I../src -I../../hap/include"

mkdir -p build out

python3 samples.py out

$CC $CFLAGS -o build/test_plist test_plist.c ../src/ap_plist.c ../src/ap_setup.c
$CC $CFLAGS -o build/test_frame test_frame.c ../src/ap_frame.c
$CC $CFLAGS -o build/test_client test_client.c ../src/ap_client.c ../src/ap_md5.c \
    ../src/ap_platform.c hap_stub.c

./build/test_plist out
./build/test_frame
./build/test_client
python3 check.py out
