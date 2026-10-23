/*
 * SRP-6a client half of HAP pair-setup, matching the reference sender
 * (doubletake pairing.go, completeSRPExchange):
 *
 *   x = SHA512(salt || SHA512("Pair-Setup" || ':' || PIN))
 *   k = SHA512(pad(N) || pad(g))
 *   A = g^a mod N
 *   u = SHA512(pad(A) || pad(B))
 *   S = (B - k*g^x mod N)^(a + u*x) mod N
 *   K = SHA512(S)                       (unpadded S, 64 bytes)
 *   M1 = SHA512(H(N)^H(g) || H(user) || salt || A || B || K)
 *   M2 = SHA512(A || M1 || K)
 *
 * Everything is SHA-512. A, B and K enter the hashes in their natural byte
 * widths; k and u are the only inputs padded to the 384-byte group width.
 */
#include "hap_internal.h"

#include <string.h>

#include "sha512.h"

/* RFC 5054 3072-bit group, transcribed verbatim from pairing.go. */
static const char srp_n_hex[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
    "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
    "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
    "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
    "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
    "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64"
    "ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B"
    "F12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
    "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31"
    "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

#define SRP_GENERATOR 5

static int hex_to_bytes(const char *hex, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t nibbles = strlen(hex);
    size_t bytes = (nibbles + 1) / 2;
    size_t index;

    if (bytes > cap) {
        return -1;
    }
    memset(out, 0, bytes);
    for (index = 0; index < nibbles; index++) {
        char chr = hex[index];
        /* Position counted from the least significant nibble. */
        size_t position = nibbles - 1 - index;
        uint8_t nibble;

        if (chr >= '0' && chr <= '9') {
            nibble = (uint8_t)(chr - '0');
        } else if (chr >= 'A' && chr <= 'F') {
            nibble = (uint8_t)(chr - 'A' + 10);
        } else if (chr >= 'a' && chr <= 'f') {
            nibble = (uint8_t)(chr - 'a' + 10);
        } else {
            return -1;
        }
        if (position % 2 == 0) {
            out[bytes - 1 - position / 2] |= nibble;
        } else {
            out[bytes - 1 - position / 2] |= (uint8_t)(nibble << 4);
        }
    }
    *out_len = bytes;
    return 0;
}

/* Length of the value once leading zero bytes are dropped. */
static size_t natural_length(const uint8_t *be, size_t len)
{
    size_t skip = 0;

    while (skip < len && be[skip] == 0) {
        skip++;
    }
    return len - skip;
}

static void sha512_add(sha512_context *hash, const void *data, size_t len)
{
    if (len != 0) {
        sha512_update(hash, (const unsigned char *)data, len);
    }
}

int hap_srp_client_start(hap_srp_t *srp, const char *pin, const uint8_t *salt, size_t salt_len,
                         const uint8_t *server_public, size_t server_public_len,
                         const uint8_t *client_secret_a)
{
    static const uint8_t zero_pad[HAP_SRP_WIDTH] = {0};
    static const uint8_t generator_bytes[1] = {SRP_GENERATOR};
    uint8_t n_bytes[HAP_SRP_WIDTH];
    size_t n_len = 0;
    uint8_t padded_b[HAP_SRP_WIDTH];
    size_t b_len;
    uint8_t inner[HAP_SRP_HASH_LEN];
    uint8_t x_hash[HAP_SRP_HASH_LEN];
    uint8_t k_hash[HAP_SRP_HASH_LEN];
    uint8_t u_hash[HAP_SRP_HASH_LEN];
    uint8_t secret_a[32];
    uint8_t s_bytes[HAP_SRP_WIDTH];
    size_t s_len;
    /* u and x are SHA-512 outputs, so u*x fits in 1024 bits and a + u*x needs no
     * more than 128 bytes. */
    uint8_t exp_bytes[2 * HAP_SRP_HASH_LEN + 32];
    size_t exp_len;
    uint8_t hn[HAP_SRP_HASH_LEN];
    uint8_t hg[HAP_SRP_HASH_LEN];
    uint8_t hu[HAP_SRP_HASH_LEN];
    uint8_t xor_hash[HAP_SRP_HASH_LEN];
    const uint8_t *a_min;
    const uint8_t *b_min;
    /* Scratch pool; reused across the steps below to keep this frame small. */
    hap_mpi_t generator;
    hap_mpi_t acc_a;
    hap_mpi_t acc_b;
    hap_mpi_t acc_x;
    hap_mpi_t acc_u;
    hap_mpi_t acc_g;
    hap_mpi_t acc_priv;
    sha512_context hash;
    size_t index;

    if (srp == NULL || salt == NULL || server_public == NULL || server_public_len == 0 ||
        server_public_len > HAP_SRP_WIDTH) {
        return -1;
    }
    if (pin == NULL) {
        pin = "";
    }

    if (hex_to_bytes(srp_n_hex, n_bytes, sizeof n_bytes, &n_len) != 0 ||
        hap_mpi_ctx_init(&srp->ctx, n_bytes, n_len) != 0) {
        HAP_LOGE("srp: cannot build the RFC 5054 group");
        return -1;
    }

    /* x = SHA512(salt || SHA512("Pair-Setup:" || PIN)) */
    sha512_init(&hash);
    sha512_add(&hash, HAP_SRP_USERNAME, sizeof HAP_SRP_USERNAME - 1);
    sha512_add(&hash, ":", 1);
    sha512_add(&hash, pin, strlen(pin));
    sha512_final(&hash, inner);

    sha512_init(&hash);
    sha512_add(&hash, salt, salt_len);
    sha512_add(&hash, inner, sizeof inner);
    sha512_final(&hash, x_hash);

    /* k = SHA512(pad(N) || pad(g)) */
    sha512_init(&hash);
    sha512_add(&hash, n_bytes, n_len);
    sha512_add(&hash, zero_pad, HAP_SRP_WIDTH - n_len);
    sha512_add(&hash, zero_pad, HAP_SRP_WIDTH - sizeof generator_bytes);
    sha512_add(&hash, generator_bytes, sizeof generator_bytes);
    sha512_final(&hash, k_hash);

    if (client_secret_a != NULL) {
        memcpy(secret_a, client_secret_a, sizeof secret_a);
    } else {
        randombytes(secret_a, sizeof secret_a);
    }

    memset(&generator, 0, sizeof generator);
    generator.d[0] = SRP_GENERATOR;
    if (hap_mpi_from_bytes(&acc_priv, secret_a, sizeof secret_a) != 0) {
        return -1;
    }
    if (hap_mpi_is_zero(&acc_priv)) {
        acc_priv.d[0] = 1; /* the reference clamps an all-zero secret to 1 */
    }

    /* A = g^a mod N, published as 384 bytes. */
    if (hap_mpi_modexp(&srp->ctx, &acc_a, &generator, secret_a, sizeof secret_a) != 0) {
        HAP_LOGE("srp: g^a failed");
        return -1;
    }
    if (hap_mpi_to_bytes_padded(&acc_a, srp->padded_a, HAP_SRP_WIDTH) != HAP_SRP_WIDTH) {
        HAP_LOGE("srp: A does not fit the group width");
        return -1;
    }
    srp->a_len = natural_length(srp->padded_a, HAP_SRP_WIDTH);
    if (srp->a_len == 0) {
        HAP_LOGE("srp: degenerate client public key");
        return -1;
    }
    a_min = srp->padded_a + (HAP_SRP_WIDTH - srp->a_len);

    /* B must be a non-zero element below N. */
    if (hap_mpi_from_bytes(&acc_b, server_public, server_public_len) != 0) {
        return -1;
    }
    if (hap_mpi_is_zero(&acc_b) || hap_mpi_cmp(&acc_b, &srp->ctx.n) >= 0) {
        HAP_LOGE("srp: server public key out of range");
        return -1;
    }
    if (hap_mpi_to_bytes_padded(&acc_b, padded_b, HAP_SRP_WIDTH) != HAP_SRP_WIDTH) {
        return -1;
    }
    b_len = natural_length(padded_b, HAP_SRP_WIDTH);
    b_min = padded_b + (HAP_SRP_WIDTH - b_len);

    /* u = SHA512(pad(A) || pad(B)) */
    sha512_init(&hash);
    sha512_add(&hash, srp->padded_a, HAP_SRP_WIDTH);
    sha512_add(&hash, zero_pad, HAP_SRP_WIDTH - b_len);
    sha512_add(&hash, b_min, b_len);
    sha512_final(&hash, u_hash);

    /* S = (B - k*g^x mod N)^(a + u*x) mod N */
    if (hap_mpi_from_bytes(&acc_x, x_hash, sizeof x_hash) != 0 ||
        hap_mpi_from_bytes(&acc_g, k_hash, sizeof k_hash) != 0 ||
        hap_mpi_from_bytes(&acc_u, u_hash, sizeof u_hash) != 0) {
        return -1;
    }
    if (hap_mpi_modexp(&srp->ctx, &acc_a, &generator, x_hash, sizeof x_hash) != 0) {
        HAP_LOGE("srp: g^x failed");
        return -1;
    }
    hap_mpi_mulmod(&srp->ctx, &acc_g, &acc_g, &acc_a); /* k * g^x */
    /* B - k*g^x mod N: a borrowing subtraction is the congruent negative
     * residue, and one addition of N brings it back since B - base > -N. */
    if (hap_mpi_sub(&acc_b, &acc_b, &acc_g) != 0) {
        hap_mpi_add(&acc_b, &acc_b, &srp->ctx.n);
    }

    hap_mpi_mulmod(&srp->ctx, &acc_u, &acc_u, &acc_x); /* u * x */
    hap_mpi_add(&acc_u, &acc_u, &acc_priv);            /* + a */
    exp_len = hap_mpi_to_bytes(&acc_u, exp_bytes, sizeof exp_bytes);
    if (exp_len == (size_t)-1 || exp_len == 0) {
        HAP_LOGE("srp: bad exponent");
        return -1;
    }
    if (hap_mpi_modexp(&srp->ctx, &acc_a, &acc_b, exp_bytes, exp_len) != 0) {
        HAP_LOGE("srp: shared secret failed");
        return -1;
    }
    s_len = hap_mpi_to_bytes(&acc_a, s_bytes, sizeof s_bytes);
    if (s_len == (size_t)-1) {
        HAP_LOGE("srp: shared secret too wide");
        return -1;
    }

    /* K = SHA512(S), natural width. */
    sha512_init(&hash);
    sha512_add(&hash, s_bytes, s_len);
    sha512_final(&hash, srp->k);

    /* M1 = SHA512(H(N) xor H(g) || H(user) || salt || A || B || K) */
    sha512_init(&hash);
    sha512_add(&hash, n_bytes, n_len);
    sha512_final(&hash, hn);
    sha512_init(&hash);
    sha512_add(&hash, generator_bytes, sizeof generator_bytes);
    sha512_final(&hash, hg);
    sha512_init(&hash);
    sha512_add(&hash, HAP_SRP_USERNAME, sizeof HAP_SRP_USERNAME - 1);
    sha512_final(&hash, hu);
    for (index = 0; index < HAP_SRP_HASH_LEN; index++) {
        xor_hash[index] = (uint8_t)(hn[index] ^ hg[index]);
    }

    sha512_init(&hash);
    sha512_add(&hash, xor_hash, sizeof xor_hash);
    sha512_add(&hash, hu, sizeof hu);
    sha512_add(&hash, salt, salt_len);
    sha512_add(&hash, a_min, srp->a_len);
    sha512_add(&hash, b_min, b_len);
    sha512_add(&hash, srp->k, sizeof srp->k);
    sha512_final(&hash, srp->m1);

    /* M2 = SHA512(A || M1 || K), the proof the server must echo in M4. */
    sha512_init(&hash);
    sha512_add(&hash, a_min, srp->a_len);
    sha512_add(&hash, srp->m1, sizeof srp->m1);
    sha512_add(&hash, srp->k, sizeof srp->k);
    sha512_final(&hash, srp->m2);

    return 0;
}

int hap_srp_verify_server_proof(const hap_srp_t *srp, const uint8_t *proof, size_t proof_len)
{
    uint8_t diff = 0;
    size_t index;

    if (srp == NULL || proof == NULL || proof_len != HAP_SRP_HASH_LEN) {
        HAP_LOGE("srp: server proof has wrong length %u", (unsigned)proof_len);
        return -1;
    }
    for (index = 0; index < HAP_SRP_HASH_LEN; index++) {
        diff |= (uint8_t)(proof[index] ^ srp->m2[index]);
    }
    if (diff != 0) {
        HAP_LOGE("srp: server proof mismatch");
        return -1;
    }
    return 0;
}
