# Host tests for the HAP component's pure-C parts. No ESP-IDF required:
#
#   make            build and run the tests
#   make vectors    regenerate vectors.h from the Python reference
#   make syntax     compile every source (including the PSA-dependent ones)
#                   against the ESP-IDF mbedTLS headers, no link step
#
# IDF_PATH must point at an ESP-IDF checkout for `make syntax`.

CC ?= clang
CFLAGS ?= -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror
VENDOR := ../third_party/ed25519
SRC := ../src
INCLUDES := -I../include -I$(SRC) -I$(VENDOR)
# The host build needs the stand-in PSA header; the syntax check must not see it.
TEST_INCLUDES := $(INCLUDES) -Ifake_psa

# fake_psa stands in for mbedTLS PSA on the host so the pair-setup, pair-verify,
# KDF and framing code can actually run; see test/fake_psa/psa/crypto.h.
OWN_SRCS := test_hap.c fake_psa.c $(SRC)/hap_tlv.c $(SRC)/hap_mpi.c $(SRC)/hap_srp.c \
            $(SRC)/hap_setup.c $(SRC)/hap_verify.c $(SRC)/hap_kdf.c \
            $(SRC)/hap_channel.c $(SRC)/hap_identity.c $(SRC)/hap_x25519.c \
            $(VENDOR)/randombytes.c
VENDOR_SRCS := $(VENDOR)/add_scalar.c $(VENDOR)/fe.c $(VENDOR)/ge.c \
               $(VENDOR)/key_exchange.c $(VENDOR)/keypair.c $(VENDOR)/sc.c \
               $(VENDOR)/sign.c $(VENDOR)/verify.c $(VENDOR)/sha512.c

BIN := hap_tests

# The vendored ed25519 sources are ref10-era C and are not warning-clean under
# -Wextra; they are third-party code and are left as they are.
VENDOR_FLAGS := -std=c99 -O2 -w

.PHONY: all run clean vectors syntax

all: run

$(BIN): $(OWN_SRCS) vectors.h $(SRC)/hap_internal.h $(wildcard ../include/*.h)
	$(CC) $(CFLAGS) $(TEST_INCLUDES) -o $@ $(OWN_SRCS) $(VENDOR_SRCS) $(VENDOR_FLAGS)

run: $(BIN)
	./$(BIN)

vectors:
	python3 reference.py > vectors.h

clean:
	rm -f $(BIN)

# Type-check the PSA-dependent sources against the real mbedTLS headers.
MBEDTLS := $(IDF_PATH)/components/mbedtls
PSA_INCLUDES := -I$(MBEDTLS)/mbedtls/tf-psa-crypto/include \
                -I$(MBEDTLS)/mbedtls/tf-psa-crypto/drivers/builtin/include \
                -I$(MBEDTLS)/mbedtls/include \
                -I$(MBEDTLS)/port/include

PSA_SRCS := $(SRC)/hap_setup.c $(SRC)/hap_verify.c $(SRC)/hap_kdf.c $(SRC)/hap_channel.c

syntax:
	@test -n "$(IDF_PATH)" || (echo "IDF_PATH is not set"; exit 1)
	@test -d "$(MBEDTLS)" || (echo "no mbedTLS component at $(MBEDTLS)"; exit 1)
	$(CC) -fsyntax-only -std=gnu17 -Wall -Wextra -Werror $(INCLUDES) $(PSA_INCLUDES) $(PSA_SRCS)
	@echo "syntax check passed"
