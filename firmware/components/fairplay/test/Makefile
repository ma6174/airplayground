# Host build of the FairPlay component (macOS, clang, CommonCrypto).
#
#   make            build and run the test
#   make build      build only
#   make san        build and run with ASan + UBSan
#   make clean
#
# The device build uses ESP-IDF's CMakeLists.txt; this Makefile only exists to
# run the same sources (minus src/fp_platform_esp.c) on the development host.

CC ?= clang
SDKROOT := $(shell xcrun --show-sdk-path 2>/dev/null)

CFLAGS := -std=c99 -O2 -g -Wall -Wextra -Wshadow -Werror \
          -I../include -I../src
LDFLAGS :=
ifneq ($(SDKROOT),)
CFLAGS += -isysroot $(SDKROOT)
LDFLAGS += -isysroot $(SDKROOT)
endif

SRCS := ../src/fp_sap.c \
        ../src/fp_md5.c \
        ../src/fp_message.c \
        ../src/fpsap.c \
        ../src/fpsap_tables.c \
        ../src/fp_vectors.c \
        ../src/fp_selftest.c \
        fp_platform_host.c \
        host_test.c

BUILD := build
BIN := $(BUILD)/host_test

.PHONY: all run build san clean

all: run

build: $(BIN)

$(BIN): $(SRCS) ../src/fp_internal.h ../include/fairplay.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

# Same code under AddressSanitizer + UndefinedBehaviorSanitizer: the SAP hash
# indexes four different tables from computed bytes, so an out-of-range index or
# an emulated shift is exactly the kind of bug that would otherwise only show up
# as a wrong key on device.
san: $(SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
		-o $(BUILD)/host_test_san $(SRCS) $(LDFLAGS)
	./$(BUILD)/host_test_san

clean:
	rm -rf $(BUILD)
