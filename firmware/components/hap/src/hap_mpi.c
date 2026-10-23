/*
 * A fixed-width big integer for SRP over the 3072-bit RFC 5054 group.
 *
 * mbedTLS 4 only exposes PSA, which has no bignum interface, so the modular
 * arithmetic pair-setup needs lives here: 96 little-endian 32-bit limbs, a
 * Montgomery multiplication and a square-and-multiply modexp. Nothing
 * allocates, and the largest working buffers are a few hundred bytes.
 *
 * The exponent in pair-setup is a + u*x (about 1024 bits), i.e. roughly 1500
 * modular multiplications per exponentiation.
 */
#include "hap_internal.h"

#include <string.h>

#define LIMBS HAP_MPI_LIMBS

static const hap_mpi_t mpi_one = {{1}};

static void mpi_zero(hap_mpi_t *r)
{
    memset(r->d, 0, sizeof r->d);
}

static void mpi_copy(hap_mpi_t *r, const hap_mpi_t *a)
{
    memcpy(r->d, a->d, sizeof r->d);
}

static int limb_bits(uint32_t value)
{
    int bits = 0;

    while (value != 0) {
        value >>= 1;
        bits++;
    }
    return bits;
}

int hap_mpi_from_bytes(hap_mpi_t *r, const uint8_t *be, size_t len)
{
    size_t index;

    if (len > HAP_MPI_BYTES) {
        return -1;
    }
    mpi_zero(r);
    for (index = 0; index < len; index++) {
        size_t bit = (len - 1 - index) * 8;
        r->d[bit / 32] |= (uint32_t)be[index] << (bit % 32);
    }
    return 0;
}

size_t hap_mpi_to_bytes(const hap_mpi_t *a, uint8_t *be, size_t cap)
{
    int top = -1;
    size_t len;
    size_t index;

    for (top = LIMBS - 1; top >= 0; top--) {
        if (a->d[top] != 0) {
            break;
        }
    }
    if (top < 0) {
        return 0;
    }
    len = (size_t)top * 4 + (size_t)((limb_bits(a->d[top]) + 7) / 8);
    if (len > cap) {
        return (size_t)-1;
    }
    for (index = 0; index < len; index++) {
        size_t bit = (len - 1 - index) * 8;
        be[index] = (uint8_t)(a->d[bit / 32] >> (bit % 32));
    }
    return len;
}

size_t hap_mpi_to_bytes_padded(const hap_mpi_t *a, uint8_t *be, size_t len)
{
    size_t minimal = hap_mpi_to_bytes(a, be, len);

    if (minimal == (size_t)-1) {
        return (size_t)-1;
    }
    if (minimal < len) {
        memmove(be + (len - minimal), be, minimal);
        memset(be, 0, len - minimal);
    }
    return len;
}

int hap_mpi_cmp(const hap_mpi_t *a, const hap_mpi_t *b)
{
    int index;

    for (index = LIMBS - 1; index >= 0; index--) {
        if (a->d[index] != b->d[index]) {
            return a->d[index] > b->d[index] ? 1 : -1;
        }
    }
    return 0;
}

bool hap_mpi_is_zero(const hap_mpi_t *a)
{
    int index;

    for (index = 0; index < LIMBS; index++) {
        if (a->d[index] != 0) {
            return false;
        }
    }
    return true;
}

int hap_mpi_sub(hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *b)
{
    uint32_t borrow = 0;
    int index;

    for (index = 0; index < LIMBS; index++) {
        uint64_t value = (uint64_t)a->d[index] - b->d[index] - borrow;
        r->d[index] = (uint32_t)value;
        borrow = (uint32_t)((value >> 32) & 1u);
    }
    return (int)borrow;
}

void hap_mpi_add(hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *b)
{
    uint64_t carry = 0;
    int index;

    for (index = 0; index < LIMBS; index++) {
        uint64_t value = (uint64_t)a->d[index] + b->d[index] + carry;
        r->d[index] = (uint32_t)value;
        carry = value >> 32;
    }
}

/* r = 2*a mod n for a < n. 2*a < 2*n, so one conditional subtraction after the
 * carry bit is enough. */
static void mpi_double_mod(hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *n)
{
    uint32_t carry = 0;
    int index;

    for (index = 0; index < LIMBS; index++) {
        uint32_t limb = a->d[index];
        r->d[index] = (limb << 1) | carry;
        carry = limb >> 31;
    }
    if (carry != 0 || hap_mpi_cmp(r, n) >= 0) {
        /* The borrow out of this subtraction consumes the carry limb. */
        hap_mpi_sub(r, r, n);
    }
}

/* -n^-1 mod 2^32, by Newton iteration (precision doubles per step). */
static uint32_t compute_n0inv(uint32_t n)
{
    uint32_t inverse = 1;
    int round;

    for (round = 0; round < 5; round++) {
        inverse *= 2u - n * inverse;
    }
    return (uint32_t)(0u - inverse);
}

int hap_mpi_ctx_init(hap_mpi_ctx_t *ctx, const uint8_t *n_be, size_t n_len)
{
    hap_mpi_t acc;
    int round;
    int top;

    if (n_be == NULL || n_len == 0 || n_len > HAP_MPI_BYTES) {
        return -1;
    }
    if (hap_mpi_from_bytes(&ctx->n, n_be, n_len) != 0) {
        return -1;
    }
    top = LIMBS - 1;
    while (top > 0 && ctx->n.d[top] == 0) {
        top--;
    }
    if ((ctx->n.d[0] & 1u) == 0 || limb_bits(ctx->n.d[top]) < 2) {
        HAP_LOGE("mpi: modulus must be odd and at least 2 bits wide");
        return -1;
    }
    ctx->n0inv = compute_n0inv(ctx->n.d[0]);

    /* R^2 mod n = 2^(2*32*LIMBS) mod n, reached by doubling. */
    mpi_copy(&acc, &mpi_one);
    for (round = 0; round < 2 * LIMBS * 32; round++) {
        mpi_double_mod(&acc, &acc, &ctx->n);
    }
    mpi_copy(&ctx->r2, &acc);
    return 0;
}

/* Montgomery reduction in place over a 2*LIMBS+2 limb buffer holding the
 * product in work[0..2*LIMBS-1]: yields product * R^-1 mod n. */
static void mont_reduce(const hap_mpi_ctx_t *ctx, hap_mpi_t *r, uint32_t *work)
{
    uint32_t high;
    int i;

    work[2 * LIMBS] = 0;
    work[2 * LIMBS + 1] = 0;

    for (i = 0; i < LIMBS; i++) {
        uint32_t factor = work[i] * ctx->n0inv;
        uint64_t carry = 0;
        int j;

        for (j = 0; j < LIMBS; j++) {
            uint64_t value = (uint64_t)factor * ctx->n.d[j] + work[i + j] + carry;
            work[i + j] = (uint32_t)value;
            carry = value >> 32;
        }
        for (j = i + LIMBS; carry != 0 && j < 2 * LIMBS + 2; j++) {
            uint64_t value = (uint64_t)work[j] + carry;
            work[j] = (uint32_t)value;
            carry = value >> 32;
        }
    }

    high = work[2 * LIMBS];
    for (i = 0; i < LIMBS; i++) {
        r->d[i] = work[LIMBS + i];
    }
    if (high != 0 || hap_mpi_cmp(r, &ctx->n) >= 0) {
        hap_mpi_sub(r, r, &ctx->n);
    }
}

/* r = a * b * R^-1 mod n, for a < n and b < n. r may alias a or b: the product
 * is fully computed before r is written. */
static void mont_mul(const hap_mpi_ctx_t *ctx, hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *b)
{
    uint32_t work[2 * LIMBS + 2];
    int i;

    memset(work, 0, sizeof work);
    for (i = 0; i < LIMBS; i++) {
        uint64_t carry = 0;
        uint32_t factor = a->d[i];
        int j;

        for (j = 0; j < LIMBS; j++) {
            uint64_t value = (uint64_t)factor * b->d[j] + work[i + j] + carry;
            work[i + j] = (uint32_t)value;
            carry = value >> 32;
        }
        work[i + LIMBS] = (uint32_t)carry;
    }
    mont_reduce(ctx, r, work);
}

/* a * R mod n. */
static void to_mont(const hap_mpi_ctx_t *ctx, hap_mpi_t *r, const hap_mpi_t *a)
{
    mont_mul(ctx, r, &ctx->r2, a);
}

void hap_mpi_mulmod(const hap_mpi_ctx_t *ctx, hap_mpi_t *r, const hap_mpi_t *a, const hap_mpi_t *b)
{
    hap_mpi_t scaled;

    to_mont(ctx, &scaled, a);
    mont_mul(ctx, r, &scaled, b);
}

int hap_mpi_modexp(const hap_mpi_ctx_t *ctx, hap_mpi_t *r, const hap_mpi_t *base,
                   const uint8_t *exp_be, size_t exp_len)
{
    hap_mpi_t base_mont;
    bool started = false;
    size_t index;

    if (exp_be == NULL || exp_len == 0 || exp_len > HAP_MPI_BYTES) {
        return -1;
    }
    if (hap_mpi_cmp(base, &ctx->n) >= 0) {
        HAP_LOGE("mpi: modexp base is not reduced");
        return -1;
    }

    to_mont(ctx, &base_mont, base);
    to_mont(ctx, r, &mpi_one); /* accumulator starts at the Montgomery form of 1 */

    for (index = 0; index < exp_len; index++) {
        int bit;

        for (bit = 7; bit >= 0; bit--) {
            int set = (exp_be[index] >> bit) & 1;

            if (!started) {
                if (set == 0) {
                    continue; /* skip leading zeros: they would square the 1 */
                }
                started = true;
            }
            mont_mul(ctx, r, r, r);
            if (set != 0) {
                mont_mul(ctx, r, r, &base_mont);
            }
        }
    }

    if (!started) {
        mpi_copy(r, &mpi_one);
        return 0;
    }
    mont_mul(ctx, r, r, &mpi_one); /* back out of the Montgomery domain */
    return 0;
}
