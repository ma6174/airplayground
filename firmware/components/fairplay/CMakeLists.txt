# FairPlay SAP component for ESP-IDF.
#
# The device build compiles only the component sources under src/; the macOS
# host harness in test/ has its own Makefile and its own platform shim
# (test/fp_platform_host.c uses CommonCrypto instead of PSA crypto), so neither
# is part of this target.
idf_component_register(
    SRCS
        "src/fp_sap.c"
        "src/fp_md5.c"
        "src/fp_message.c"
        "src/fpsap.c"
        "src/fpsap_tables.c"
        "src/fp_platform_esp.c"
        "src/fp_selftest.c"
        "src/fp_vectors.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls
)
