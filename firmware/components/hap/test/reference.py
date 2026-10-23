#!/usr/bin/env python3
"""Reference implementation of the HAP pair-setup SRP-6a exchange.

This mirrors doubletake's internal/airplay/pairing.go (completeSRPExchange) and
emits test/vectors.h for the C host tests. Run it from the test directory:

    python3 reference.py > vectors.h

Everything below uses hashlib only: no third-party packages, so the expected
values can be re-derived anywhere.
"""

import hashlib
import hmac
import sys

# RFC 5054 3072-bit group, identical to the literal in pairing.go.
N_HEX = (
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
    "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF"
)

N = int(N_HEX, 16)
G = 5
WIDTH = 384  # 3072 bits: the SRP width used on the wire
USERNAME = b"Pair-Setup"


def h(*parts):
    digest = hashlib.sha512()
    for part in parts:
        digest.update(part)
    return digest.digest()


def be(value):
    """Minimal big-endian representation, exactly like Go's big.Int.Bytes()."""
    return value.to_bytes((value.bit_length() + 7) // 8, "big")


def pad(data, width=WIDTH):
    return b"\x00" * (width - len(data)) + data


def srp_client(pin, salt, b_server, a_client):
    """Reproduces completeSRPExchange from pairing.go."""
    x = int.from_bytes(h(salt, h(USERNAME + b":" + pin)), "big")
    k = int.from_bytes(h(pad(be(N)), pad(be(G))), "big")
    A = pow(G, a_client, N)
    B = pow(G, b_server, N)
    u = int.from_bytes(h(pad(be(A)), pad(be(B))), "big")
    S = pow((B - k * pow(G, x, N)) % N, a_client + u * x, N)
    K = h(be(S))

    # M1 = H(H(N) xor H(g), H(I), s, A, B, K), natural byte widths.
    hxor = bytes(left ^ right for left, right in zip(h(be(N)), h(be(G))))
    m1 = h(hxor, h(USERNAME), salt, be(A), be(B), K)
    m2 = h(be(A), m1, K)

    return {"x": x, "k": k, "A": A, "B": B, "u": u, "S": S, "K": K, "M1": m1, "M2": m2}


def c_array(name, data):
    body = ", ".join("0x%02x" % byte for byte in data)
    return "static const uint8_t %s[%d] = {%s};\n" % (name, len(data), body)


# ------------------------------------------------------- ChaCha20-Poly1305
#
# A self-contained reference so the C stand-in for PSA can be compared against
# something that is not itself. Follows RFC 8439.

def _rotl32(value, count):
    return ((value << count) | (value >> (32 - count))) & 0xFFFFFFFF


def chacha20_block(key, counter, nonce):
    state = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    state += [int.from_bytes(key[i:i + 4], "little") for i in range(0, 32, 4)]
    state.append(counter & 0xFFFFFFFF)
    state += [int.from_bytes(nonce[i:i + 4], "little") for i in range(0, 12, 4)]
    work = list(state)

    for _ in range(10):
        for (a, b, c, d) in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                             (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
            work[a] = (work[a] + work[b]) & 0xFFFFFFFF
            work[d] = _rotl32(work[d] ^ work[a], 16)
            work[c] = (work[c] + work[d]) & 0xFFFFFFFF
            work[b] = _rotl32(work[b] ^ work[c], 12)
            work[a] = (work[a] + work[b]) & 0xFFFFFFFF
            work[d] = _rotl32(work[d] ^ work[a], 8)
            work[c] = (work[c] + work[d]) & 0xFFFFFFFF
            work[b] = _rotl32(work[b] ^ work[c], 7)

    out = b""
    for i in range(16):
        out += ((work[i] + state[i]) & 0xFFFFFFFF).to_bytes(4, "little")
    return out


def chacha20_xor(data, key, counter, nonce):
    out = bytearray()
    for offset in range(0, len(data), 64):
        block = chacha20_block(key, counter + offset // 64, nonce)
        chunk = data[offset:offset + 64]
        out += bytes(p ^ q for p, q in zip(chunk, block))
    return bytes(out)


def poly1305(msg, key):
    r = int.from_bytes(key[:16], "little") & 0x0FFFFFFC0FFFFFFC0FFFFFFC0FFFFFFF
    s = int.from_bytes(key[16:], "little")
    prime = (1 << 130) - 5
    acc = 0
    for offset in range(0, len(msg), 16):
        block = msg[offset:offset + 16]
        acc = (acc + int.from_bytes(block + b"\x01", "little")) * r % prime
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def aead_seal(key, nonce, aad, plaintext):
    otk = chacha20_block(key, 0, nonce)[:32]
    ciphertext = chacha20_xor(plaintext, key, 1, nonce)
    padded_aad = aad + b"\x00" * (-len(aad) % 16)
    padded_ct = ciphertext + b"\x00" * (-len(ciphertext) % 16)
    mac_input = (padded_aad + padded_ct + len(aad).to_bytes(8, "little") +
                 len(ciphertext).to_bytes(8, "little"))
    return ciphertext + poly1305(mac_input, otk)


def aead_open(key, nonce, aad, sealed):
    ciphertext, tag = sealed[:-16], sealed[-16:]
    otk = chacha20_block(key, 0, nonce)[:32]
    padded_aad = aad + b"\x00" * (-len(aad) % 16)
    padded_ct = ciphertext + b"\x00" * (-len(ciphertext) % 16)
    mac_input = (padded_aad + padded_ct + len(aad).to_bytes(8, "little") +
                 len(ciphertext).to_bytes(8, "little"))
    if poly1305(mac_input, otk) != tag:
        return None
    return chacha20_xor(ciphertext, key, 1, nonce)


def hkdf_sha512(ikm, salt, info, length):
    prk = hmac.new(salt, ikm, hashlib.sha512).digest()
    output, block = b"", b""
    counter = 1
    while len(output) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha512).digest()
        output += block
        counter += 1
    return output[:length]


# Deterministic SRP transcripts: (name, pin, salt, server b, client a).
SRP_CASES = [
    ("transient", b"",
     bytes.fromhex("00112233445566778899aabbccddeeff"),
     bytes.fromhex("11" * 32),
     bytes.fromhex("22" * 32)),
    ("pin", b"1234",
     bytes.fromhex("ffeeddccbbaa99887766554433221100"),
     bytes.fromhex("0123456789abcdef" * 4),
     bytes.fromhex("fedcba9876543210" * 4)),
]

# Modular exponentiation vectors: (name, modulus, base, exponent).
MPI_VECTORS = [
    ("group_g_x", N, G, 0x5fb1c3d3a2f0e1d4c6b7a8998877665544332211ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100ffeeddccbbaa998877665544332211),
    ("group_random", N, pow(2, 255) - 19, pow(3, 700)),
    ("p2048", 2 ** 2048 - 1942289, pow(7, 400), pow(5, 333)),
    ("p1024", 2 ** 1024 - 105, pow(11, 250), pow(13, 211)),
    ("p256", 2 ** 256 - 189, 0xdeadbeefcafebabe1234567890abcdef, 0x0123456789abcdef),
    ("p64", 2 ** 64 - 59, 0x1234567890abcdef, 0xffffffffffffff43),
    ("base_one", N, 1, 12345),
    ("exp_one", N, pow(2, 3000) + 12345, 1),
]


def main():
    out = ["/* Generated by reference.py -- do not edit. */\n",
           "#pragma once\n\n#include <stdint.h>\n\n"]

    summary = []
    for name, pin, salt, b_server, a_client in SRP_CASES:
        result = srp_client(pin, salt, int.from_bytes(b_server, "big"),
                            int.from_bytes(a_client, "big"))
        prefix = "srp_%s_" % name
        out.append("/* SRP case %r, pin %r */\n" % (name, pin.decode()))
        out.append(c_array(prefix + "salt", salt))
        # The wire value is the server's SRP public key B = g^b mod N.
        out.append(c_array(prefix + "B", pad(be(result["B"]))))
        out.append(c_array(prefix + "a", a_client))
        # The receiver's SRP private value, used by the test that plays the
        # receiver's half of the exchange.
        out.append(c_array(prefix + "secret_b", b_server))
        out.append(c_array(prefix + "pad_A", pad(be(result["A"]))))
        out.append(c_array(prefix + "K", result["K"]))
        out.append(c_array(prefix + "M1", result["M1"]))
        out.append(c_array(prefix + "M2", result["M2"]))
        out.append("\n")

        summary.append("[%s]" % name)
        summary.append("  A  = %s" % be(result["A"]).hex())
        summary.append("  u  = %064x" % result["u"])
        summary.append("  S  = %s" % be(result["S"]).hex())
        summary.append("  K  = %s" % result["K"].hex())
        summary.append("  M1 = %s" % result["M1"].hex())
        summary.append("  M2 = %s" % result["M2"].hex())

    out.append("/* Montgomery context constants for the SRP group. */\n")
    n0inv = (-pow(N, -1, 1 << 32)) % (1 << 32)
    r2 = pow(2, 2 * 3072, N)
    out.append(c_array("mpi_r2", pad(be(r2))))
    out.append("static const uint32_t mpi_n0inv = 0x%08xU;\n\n" % n0inv)
    summary.append("[montgomery]")
    summary.append("  n0inv = 0x%08x" % n0inv)
    summary.append("  R^2   = %s" % be(r2).hex())

    out.append("typedef struct {\n"
               "    const char *name;\n"
               "    uint16_t mod_len, base_len, exp_len, expected_len;\n"
               "    const uint8_t *mod, *base, *exp, *expected;\n"
               "} mpi_vector_t;\n\n")

    summary.append("[modexp]")
    entries = []
    for name, modulus, base, exp in MPI_VECTORS:
        expected = pow(base, exp, modulus)
        out.append(c_array("mpi_%s_mod" % name, be(modulus)))
        out.append(c_array("mpi_%s_base" % name, be(base)))
        out.append(c_array("mpi_%s_exp" % name, be(exp)))
        out.append(c_array("mpi_%s_expected" % name, be(expected)))
        entries.append('    {"%s", sizeof(mpi_%s_mod), sizeof(mpi_%s_base), '
                       'sizeof(mpi_%s_exp), sizeof(mpi_%s_expected), '
                       'mpi_%s_mod, mpi_%s_base, mpi_%s_exp, mpi_%s_expected}' % (
                           name, name, name, name, name, name, name, name, name))
        summary.append("  %-13s mod=%d bits exp=%d bits -> %s...(%d bytes)" % (
            name, modulus.bit_length(), exp.bit_length(), be(expected).hex()[:32],
            len(be(expected))))

    out.append("static const mpi_vector_t mpi_vectors[] = {\n")
    out.append(",\n".join(entries))
    out.append("\n};\n")
    out.append("#define MPI_VECTOR_COUNT %d\n\n" % len(MPI_VECTORS))

    # ---- AEAD and KDF vectors -------------------------------------------------
    # Sanity-check the Python AEAD against RFC 8439 section 2.8.2 before it is
    # used as an oracle for the C side.
    rfc_key = bytes(range(0x80, 0xA0))
    rfc_nonce = bytes.fromhex("070000004041424344454647")
    rfc_aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
    rfc_plain = (b"Ladies and Gentlemen of the class of '99: If I could offer you "
                 b"only one tip for the future, sunscreen would be it.")
    rfc_expected = bytes.fromhex(
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116"
        "1ae10b594f09e26a7e902ecbd0600691")
    if aead_seal(rfc_key, rfc_nonce, rfc_aad, rfc_plain) != rfc_expected:
        raise SystemExit("Python ChaCha20-Poly1305 disagrees with RFC 8439 2.8.2")
    summary.append("[aead] RFC 8439 2.8.2 self-check passed")

    # The component addresses nonces as 4 zero bytes || LE64(counter), so pick
    # counters whose nonce is reproducible on either side.
    frame_key = bytes(range(32))
    frame_aad = b"\x34\x12"
    frame_plain = bytes((i * 37 + 11) & 0xFF for i in range(48))
    frame_nonce = b"\x00" * 4 + (0x0102030405060708).to_bytes(8, "little")
    frame_sealed = aead_seal(frame_key, frame_nonce, frame_aad, frame_plain)
    out.append("/* AEAD: nonce = 4 zero bytes || LE64(0x0102030405060708), AAD = 34 12 */\n")
    out.append(c_array("aead_frame_key", frame_key))
    out.append(c_array("aead_frame_aad", frame_aad))
    out.append(c_array("aead_frame_plain", frame_plain))
    out.append(c_array("aead_frame_sealed", frame_sealed))
    out.append("static const uint64_t aead_frame_nonce = 0x0102030405060708ULL;\n\n")
    summary.append("[aead] counter nonce vector: %d bytes sealed" % len(frame_sealed))

    label_key = bytes((0xF0 - i) & 0xFF for i in range(32))
    label_plain = bytes((i * 5 + 1) & 0xFF for i in range(100))
    label_sealed = aead_seal(label_key, b"\x00" * 4 + b"PS-Msg05", b"", label_plain)
    out.append("/* AEAD: label nonce 0000 || \"PS-Msg05\", no AAD */\n")
    out.append(c_array("aead_label_key", label_key))
    out.append(c_array("aead_label_plain", label_plain))
    out.append(c_array("aead_label_sealed", label_sealed))
    out.append("\n")
    summary.append("[aead] label nonce vector: %d bytes sealed" % len(label_sealed))

    # ---- HKDF-SHA512 and the channel keys ------------------------------------
    hkdf_ikm = bytes((i * 3 + 7) & 0xFF for i in range(64))
    hkdf_out = hkdf_sha512(hkdf_ikm, b"Control-Salt", b"Control-Write-Encryption-Key", 32)
    out.append(c_array("hkdf_ikm", hkdf_ikm))
    out.append(c_array("hkdf_expected", hkdf_out))
    summary.append("[hkdf] Control-Write-Encryption-Key = %s" % hkdf_out.hex())

    shared = bytes((i * 17 + 3) & 0xFF for i in range(32))
    stream_id = 1234567890123
    control_write = hkdf_sha512(shared, b"Control-Salt", b"Control-Write-Encryption-Key", 32)
    control_read = hkdf_sha512(shared, b"Control-Salt", b"Control-Read-Encryption-Key", 32)
    event_write = hkdf_sha512(shared, b"Events-Salt", b"Events-Read-Encryption-Key", 32)
    event_read = hkdf_sha512(shared, b"Events-Salt", b"Events-Write-Encryption-Key", 32)
    data_key = hkdf_sha512(shared, b"DataStream-Salt%d" % stream_id,
                           b"DataStream-Output-Encryption-Key", 32)
    out.append("static const uint64_t channel_stream_id = 1234567890123ULL;\n")
    out.append(c_array("channel_shared", shared))
    out.append(c_array("channel_control_write", control_write))
    out.append(c_array("channel_control_read", control_read))
    out.append(c_array("channel_event_write", event_write))
    out.append(c_array("channel_event_read", event_read))
    out.append(c_array("channel_data_key", data_key))
    summary.append("[kdf] control write %s" % control_write.hex())
    summary.append("[kdf] control read  %s" % control_read.hex())
    summary.append("[kdf] event write   %s" % event_write.hex())
    summary.append("[kdf] event read    %s" % event_read.hex())
    summary.append("[kdf] data (id %d) %s" % (stream_id, data_key.hex()))

    # The header goes to stdout; the human-readable digest goes to stderr so
    # `python3 reference.py > vectors.h` stays clean.
    print("\n".join(summary), file=sys.stderr)
    return "".join(out)


if __name__ == "__main__":
    sys.stdout.write(main())
