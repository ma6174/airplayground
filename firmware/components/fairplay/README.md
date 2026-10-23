# fairplay — FairPlay SAP handshake

AirPlay v3 locks the audio stream key with a two-message `/fp-setup` exchange.
This component implements the sender side of that exchange: it builds m1,
answers m2 with m3, verifies the m4 confirmation and wraps the stream key into
the 72-byte `ekey` record.

```c
fp_session_init(&session, random_cb, ctx);      // localSAP = 00 01 || 126 random bytes
fp_message1(&session, m1);                      // POST /fp-setup
fp_exchange_m3(&session, m2, m2_len, m3);       // POST /fp-setup
fp_confirm_m4(&session, m4, m4_len);
fp_wrap_key(&session, raw_key, random_cb, ctx, ekey);
fp_stream_key(raw_key, pair_secret, secret_len, mix, out);
```

The public contract is `include/fairplay.h`; everything else in this directory is
an implementation detail. Every function returns 0 on success and a negative
value otherwise (the code list lives in `src/fp_internal.h`).

## Layout

| Path | Contents | Origin |
|---|---|---|
| `src/fp_sap.c` | SAP hash: 840-round scramble, straight-line circuit, 256-step diffusion | `fairplay_sap.go` |
| `src/fp_md5.c` | modified MD5 compression, three schedule mutations | `fairplay_md5.go` |
| `src/fp_message.c` | SAS-CBC message cipher for modes 0..3, AES-128 block helpers | `fairplay_message.go` |
| `src/fpsap.c` | white-box networks, descriptor, exchange, FPLY records, ekey wrapping, session | `fpsap.go`, `fairplay_crypto.go` |
| `src/fpsap_tables.c` | 13 permutation bases plus the lookup triples of both networks | `fpsap_tables.go` (generated) |
| `src/fp_platform_esp.c` | `fp_sha512()` and `fp_hmac_sha1()` on PSA crypto | device only |
| `src/fp_selftest.c` | `fp_selftest()` and the per-category runners | — |
| `src/fp_vectors.c` | all self-test vectors | generated from the Go reference |
| `src/fp_internal.h` | private interface shared by the files above | — |
| `src/fp_data_*.inc` | scalar constants, included by exactly one translation unit each | generated |
| `test/` | macOS host harness (CommonCrypto) | test only |
| `tools/` | the three generators | — |

Nothing in `test/` or any host code is part of the ESP-IDF build: `CMakeLists.txt`
lists only `src/*.c` and uses `fp_platform_esp.c` there.

## Data files are generated, never typed

No number in the generated files is typed by hand. Both pipelines start from the
Go implementation (`FAIRPLAY_GO_DIR`, default `~/airplay/doubletake/internal/airplay`):

```sh
./tools/export_data.sh          # Go constants -> tools/fairplay_data.h
python3 tools/gen_tables.py      # header -> src/fpsap_tables.c + src/fp_data_*.inc
./tools/gen_vectors.sh           # Go reference -> src/fp_vectors.c
```

`export_data.sh` and `gen_vectors.sh` copy the six Go sources into a scratch
module so the exporters can call package internals; `gen_tables.py` expands every
table and compares the result against the checksum the Go tests publish
(90128 bytes / `28d0986a…2b43`) before writing anything, so a wrong digit cannot
reach the component. `gen_vectors.sh` re-checks the published mode-3 vector and
the captured ekey while generating, so a change on the Go side surfaces
immediately.

## Host test

```sh
cd test && make          # clang + CommonCrypto, ~1 s
cd test && make san      # same under ASan + UBSan
```

The host build compiles the device sources unchanged except for the platform
file, which it replaces with `test/fp_platform_host.c`. It additionally expands
the white-box tables and compares their SHA-256 against the published value, and
prints a PASS/FAIL count per self-test category. The device build calls the same
`fp_selftest_run()`, so both paths exercise identical code.

`fp_selftest()` returns 0 when every published vector is reproduced and prints
the first failing check of each category otherwise.

## Behaviour worth knowing

* **Session lifetime.** One `fp_session_t` spans m1, m3 and the key wrap; it
  holds the local SAP, the receiver SAP recovered from m2, and the m3 record.
  `fp_wrap_key()` refuses to run before `fp_exchange_m3()` succeeded.
* **Entropy.** `fp_session_init()` needs 126 bytes and `fp_wrap_key()` 16 bytes
  for the ekey mask. Both callbacks must fill the buffer or return non-zero;
  a failing callback aborts the call and leaves the session untouched.
* **Stream key.** With `mix_pair_secret` the key is `SHA-512(raw || secret)[0:16]`
  (HAP pair-verify). Without it, or if the secret is empty/oversized or SHA-512
  fails, the raw key is copied through, which is what a physical AppleTV3
  expects.
* **Mode handling.** The receiver picks mode 0..3 in m2 and everything after that
  uses it; an out-of-range mode is rejected rather than clamped.
* **Fixed buffers and stack.** The largest single buffer is the 320-byte
  descriptor/KDF record. Per-function frames (`clang -fstack-usage`, -O2) give a
  worst handshake path of about 1.6 KiB -
  `fp_wrap_key` 432 B → `fp_wrapping_key` 544 B → `fp_sap_hash` 560 B - which
  fits any ESP-IDF task. `fp_selftest()` is heavier: `run_reject` 1536 B and
  `run_chain` 1488 B on top of `fp_exchange_m3`/`fp_wrap_key`/`fp_sap_hash`, so
  about 3.5 KiB. Run the self test from a task with ≥8 KiB of stack (or raise the
  caller's), not from a minimal one.
* **PSA.** `fp_platform_esp.c` calls `psa_crypto_init()` on entry (it is
  idempotent) and imports the 20-byte MAC key as a volatile HMAC key for a single
  `psa_mac_compute()` call. Needs `CONFIG_MBEDTLS_PSA_CRYPTO_C`, SHA-1, SHA-512
  and HMAC — all enabled by default in ESP-IDF. Nothing else from `mbedtls` is
  used; MD5 and AES are implemented in the component because FairPlay's variants
  are not the standard algorithms.
* **Flash.** The tables are ~4.4 KiB; `fp_vectors.c` plus `fp_selftest.c` add
  ~38 KiB that a production image can drop by removing them from `CMakeLists.txt`.

## Differences from the plain C transcription

The component is a direct migration of the validated reference transcription, and
keeps its statement order verbatim for the SAP circuit. The deliberate changes:

* **No in-place cipher use.** The mode-3 reverse CBC walk exists in the reference
  so decrypting could reuse the message buffer. Here the mode is passed
  explicitly and every call uses separate buffers; the reverse walk is kept
  because it is what the protocol's implementations do (and it is provably
  equivalent, see the mutation note below).
* **Explicit mode plumbing.** The Go code smuggles the mode into a temporary
  144-byte record (`message[12] = mode`) so the block cipher can read it back.
  The C API takes `mode` as an argument, and `fp_exchange_m3()` passes
  `m2[13]`/`m3[12]` explicitly.
* **Forward AES S-box is data, not a mutable global.** Both the reference and
  the Go source derive it from the inverse S-box at start-up. `gen_tables.py`
  derives it while generating and emits it as `const`, so there is no writable
  table and no first-call initialisation race.
* **Standard AES comes from the component's own round primitives.** ekey[56:72]
  is a plain AES-128 block; instead of carrying a second AES implementation the
  key expansion and block encryption reuse the S-box/mix-column code from
  `fp_message.c`. The Rcon sequence is generated (xtime) instead of tabulated.
* **HMAC-SHA1 is implemented.** The reference transcription leaves ekey[36:56]
  zeroed; the component computes it through PSA and the self test compares the
  full 72-byte record.
* **Validation instead of assertions.** Record headers, declared lengths, the m2
  marker, modes and the m4 echo are all checked and reported through return
  codes; the reference compared buffers inside a test main.
* **`fp_wrapping_key()` checks the mode itself** (the Go helper relied on its
  caller), and `fp_stream_key()` caps the pair secret at 64 bytes.
* **The SAP circuit lives in `sap_circuit()`** rather than inline in the hash,
  with the porting hazards (operator precedence, uint32 wrap-around, int
  promotion points, `rotateOrZero` vs `rotl8`, two-network asymmetry, byte order)
  documented at the statements they affect.

## Verification status

Covered by the self test (474 checks, all passing on the host): 65 SAP hash
vectors, the three MD5 mutations, four message modes with re-encryption, seven
descriptor/exchange cases, four wrapping keys, five complete
m1→m3→m4→ekey chains (HMAC-SHA1 and AES block included), the captured
unwrap vector, the white-box table checksum, and the rejection paths.

Mutation testing was used to confirm the checks bite: changing the
`(aux[4] & 0xca) >> 1` grouping, the `wide_seed` truncation point, the
`rotate_or_zero` semantics, the round-31 mutation position, the network table
indexing or mask placement, the digest32 mutation type, the descriptor
composition order, the AES Rcon polynomial, or deriving the ekey MAC key from the
wrapping key are each caught by multiple categories. Rewriting mode 3's reverse
CBC walk into a forward walk is *not* caught, which is the expected equivalence.

Not yet verified:

* `fp_platform_esp.c` has been syntax-checked against the ESP-IDF PSA headers
  but not executed on hardware; the PSA-backed HMAC/SHA-512 paths are exercised
  on the host only through the CommonCrypto shim.
* No ESP-IDF build or `idf.py` run was performed from here (by instruction), so
  component requirements and linker behaviour are unverified. If the IDF version
  splits PSA into an `esp_psa_crypto` component, add it to `REQUIRES`.
* Nothing has been tested against a real receiver: a live `/fp-setup` exchange
  with an Apple device is the remaining integration step.
