/*
 * X25519 on top of the vendored ed25519 field arithmetic.
 *
 * ed25519_key_exchange() already implements a clamped Montgomery ladder, but it
 * enters the ladder through the Edwards-to-Montgomery map: it reads its peer key
 * argument as an Edwards y coordinate and uses u = (y+1)/(1-y). X25519 peers
 * publish the Montgomery u itself, so the peer key is mapped back first:
 *
 *     y' = (u-1)/(u+1)  =>  (y'+1)/(1-y') = u
 *
 * After that substitution the shared secret is exactly X25519(clamp(private),
 * peer_u), including the same scalar clamping and the same all-zero output check
 * for low-order points.
 */
#include "hap_internal.h"

#include <string.h>

#include "ed25519.h"
#include "fe.h"

#define X25519_BASE_U 9

static void montgomery_to_edwards(uint8_t y[32], const uint8_t u[32])
{
    fe u_fe;
    fe one;
    fe numerator;
    fe denominator;

    fe_frombytes(u_fe, u); /* ignores bit 255, like X25519 does */
    fe_1(one);
    fe_sub(numerator, u_fe, one);
    fe_add(denominator, u_fe, one);
    fe_invert(denominator, denominator);
    fe_mul(numerator, numerator, denominator);
    fe_tobytes(y, numerator);
}

static void ladder(uint8_t out[32], const uint8_t private_key[32], const uint8_t u[32])
{
    uint8_t y[32];

    montgomery_to_edwards(y, u);
    ed25519_key_exchange(out, y, private_key);
}

void hap_x25519_public_key(uint8_t public_key[32], const uint8_t private_key[32])
{
    uint8_t base_u[32] = {0};

    base_u[0] = X25519_BASE_U;
    ladder(public_key, private_key, base_u);
}

int hap_x25519_shared(uint8_t shared[32], const uint8_t private_key[32], const uint8_t peer[32])
{
    uint8_t secret[32];
    uint8_t accumulator = 0;
    size_t index;

    ladder(secret, private_key, peer);
    for (index = 0; index < sizeof secret; index++) {
        accumulator |= secret[index];
    }
    if (accumulator == 0) {
        HAP_LOGE("x25519: peer key produced an all-zero secret");
        return -1;
    }
    memcpy(shared, secret, sizeof secret);
    return 0;
}
