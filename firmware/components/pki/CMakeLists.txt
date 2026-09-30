# pki component: device-side PKI provisioning verification (Tier 2 trust).
#
# The Tier 2 trust anchors (Tier 0 Ed25519 pubkey, Tier 1 P-256 pubkey,
# cert serial + expiry) are BAKED at build time by
#   firmware/tools/bake_tier2_keys.py
# which verifies the Tier 0 signature over the Tier 1 cert BEFORE baking.
#
# There is deliberately NO default / checked-in tier2_keys.h: baking an
# unverified pubkey would silently ship an untrusted trust anchor. If
# the header is missing, configuration fails LOUDLY with instructions.
#
# Manual pre-build step (documented in INTEGRATION.md):
#   TIER0_PUBKEY_HEX=<64 hex chars> \
#     firmware/tools/bake_tier2_keys.py \
#       --tier0-pubkey-hex "$TIER0_PUBKEY_HEX" \
#       --cert-file /path/to/tier1-cert.json
# then: idf.py build
#
# Crypto deps: mbedtls (ECDSA P-256 verify), seal (Monocypher Ed25519
# via its public include dir "optional/monocypher-ed25519.h", and the
# seal_dev_nvs_*_sync broker APIs for the follow-up persistence task),
# json (cJSON, same parser link.c uses).

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/tier2_keys.h")
    message(FATAL_ERROR
        "PKI: tier2_keys.h is missing — Tier 2 trust anchors were never baked.\n"
        "Refusing to build firmware without verified trust anchors.\n"
        "Run BEFORE idf.py build:\n"
        "  TIER0_PUBKEY_HEX=<64 hex chars of the Tier 0 Ed25519 pubkey> \\\n"
        "    ${CMAKE_CURRENT_LIST_DIR}/../../tools/bake_tier2_keys.py \\\n"
        "      --tier0-pubkey-hex \"$TIER0_PUBKEY_HEX\" \\\n"
        "      --cert-file /path/to/tier1-cert.json\n"
        "(or --tier1-pubkey-url <GET /tier1/pubkey>).\n"
        "The script verifies the Tier 0 signature and the cert window\n"
        "before baking; it exits non-zero rather than baking a bad cert.\n"
        "For a crypto-logic smoke test with ephemeral keys:\n"
        "  ${CMAKE_CURRENT_LIST_DIR}/../../tools/bake_tier2_keys.py --self-test")
endif()

idf_component_register(SRCS "pki_verify.c"
                       INCLUDE_DIRS "."
                       REQUIRES seal mbedtls json)
