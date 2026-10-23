/*
 * Host test for the FairPlay component (macOS, clang, CommonCrypto).
 *
 * Two things happen here that the on-device self test cannot do:
 *
 *   1. the white-box tables are expanded and hashed, so a single mistyped digit
 *      in the generated table file is caught (the Go tests publish the expected
 *      byte count and SHA-256);
 *   2. every self-test category runs separately and prints its PASS/FAIL count.
 *
 * Everything else (SAP hash, modified MD5, message cipher, descriptor,
 * exchange, ekey wrapping) is the exact device code path: fp_selftest_run()
 * lives in the component.
 */
#include <CommonCrypto/CommonDigest.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fairplay.h"
#include "fp_internal.h"

/* Published by the Go table checksum test. */
#define EXPECTED_TABLE_BYTES 90128
#define EXPECTED_TABLE_SHA256 "28d0986abebe30458348dfa2957aa1d52d6f3ad5a9468c5d8a9c4139b7ca2b43"

typedef struct {
    const fp_lookup (*round)[16];
    const fp_lookup (*mix)[4];
    const fp_lookup *final;
} host_network_tables;

static void hash_expanded_table(CC_SHA256_CTX *ctx, const fp_lookup *lookup, bool use_mix_base, int *written)
{
    uint8_t expanded[256];
    for (int value = 0; value < 256; value++) {
        expanded[value] = use_mix_base
                              ? (uint8_t)(fpsap_mix_bases[lookup->table][value ^ lookup->in_xor] ^ lookup->out_xor)
                              : (uint8_t)(fpsap_substitution_bases[lookup->table][value ^ lookup->in_xor] ^ lookup->out_xor);
    }
    CC_SHA256_Update(ctx, expanded, sizeof expanded);
    *written += (int)sizeof expanded;
}

static int check_table_checksum(void)
{
    static const host_network_tables networks[2] = {
        {fpsap_first_round, fpsap_first_mix, fpsap_first_final},
        {fpsap_second_round, fpsap_second_mix, fpsap_second_final},
    };
    CC_SHA256_CTX ctx;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    int written = 0;

    CC_SHA256_Init(&ctx);
    CC_SHA256_Update(&ctx, fpsap_first_input_mask, sizeof fpsap_first_input_mask);
    written += (int)sizeof fpsap_first_input_mask;

    for (int network = 0; network < 2; network++) {
        for (int bank = 0; bank < 9; bank++) {
            for (int column = 0; column < 16; column++) {
                hash_expanded_table(&ctx, &networks[network].round[bank][column], false, &written);
            }
        }
        for (int column = 0; column < 4; column++) {
            /* Mix tables are expanded value-major: four output bytes per input. */
            uint8_t expanded[256 * 4];
            for (int value = 0; value < 256; value++) {
                for (int output_byte = 0; output_byte < 4; output_byte++) {
                    const fp_lookup *lookup = &networks[network].mix[column][output_byte];
                    expanded[value * 4 + output_byte] =
                        (uint8_t)(fpsap_mix_bases[lookup->table][value ^ lookup->in_xor] ^ lookup->out_xor);
                }
            }
            CC_SHA256_Update(&ctx, expanded, sizeof expanded);
            written += (int)sizeof expanded;
        }
        for (int column = 0; column < 16; column++) {
            hash_expanded_table(&ctx, &networks[network].final[column], false, &written);
        }
    }
    CC_SHA256_Final(digest, &ctx);

    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }

    bool ok = written == EXPECTED_TABLE_BYTES && strcmp(hex, EXPECTED_TABLE_SHA256) == 0;
    printf("  %-28s %s  %d bytes, sha256 %s\n", "white-box tables", ok ? "PASS" : "FAIL", written, hex);
    if (!ok) {
        printf("  %-28s       want %d bytes, sha256 %s\n", "", EXPECTED_TABLE_BYTES, EXPECTED_TABLE_SHA256);
    }
    return ok ? 0 : 1;
}

int main(void)
{
    fp_test_stats stats[FP_TEST_CATEGORY_COUNT];

    printf("fairplay component host test (clang + CommonCrypto)\n");

    int failures = 0;
    failures += check_table_checksum();

    unsigned checked_total = 0;
    unsigned failed_total = 0;
    const unsigned all_categories = (1u << FP_TEST_CATEGORY_COUNT) - 1u;
    if (fp_selftest_run(all_categories, stats) != FP_OK) {
        failures += 1;
    }
    for (int category = 0; category < FP_TEST_CATEGORY_COUNT; category++) {
        printf("  %-28s %s  %u/%u checks\n", fp_test_category_name((fp_test_category)category),
               stats[category].failed == 0 ? "PASS" : "FAIL", stats[category].checked - stats[category].failed,
               stats[category].checked);
        checked_total += stats[category].checked;
        failed_total += stats[category].failed;
    }
    printf("  %-28s %u checks, %u failures\n", "total", checked_total, failed_total);

    /* The public entry point must agree with the per-category run. */
    int rc = fp_selftest();
    printf("  %-28s %s  fp_selftest() returned %d\n", "public fp_selftest()", rc == 0 ? "PASS" : "FAIL", rc);
    if (rc != 0) {
        failures += 1;
    }

    printf("%s\n", (failures == 0 && failed_total == 0) ? "ALL PASS" : "FAILURES");
    return (failures == 0 && failed_total == 0) ? 0 : 1;
}
