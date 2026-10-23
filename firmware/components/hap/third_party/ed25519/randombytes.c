/*
 * Entropy for the HAP key material (identity seeds, SRP ephemerals, X25519
 * private keys). The vendored ed25519 sources expect a NaCl-style
 * randombytes(); on the device that is the hardware CSPRNG, and the host tests
 * read /dev/urandom.
 */
#include <stddef.h>

#if defined(ESP_PLATFORM)

#include "esp_random.h"

void randombytes(unsigned char *buf, size_t len)
{
    esp_fill_random(buf, len);
}

#else

#include <stdio.h>
#include <stdlib.h>

void randombytes(unsigned char *buf, size_t len)
{
    FILE *source = fopen("/dev/urandom", "rb");

    if (source == NULL) {
        abort();
    }
    if (fread(buf, 1, len, source) != len) {
        fclose(source);
        abort();
    }
    fclose(source);
}

#endif
