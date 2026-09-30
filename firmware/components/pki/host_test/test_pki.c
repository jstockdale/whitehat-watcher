/* Host unit test for components/pki/pki_verify.c (pure logic side).
 *
 * Build & run: ./run.sh
 *
 * Strategy: compile pki_verify.c for the host with -DPKI_HOST_TEST
 * (ESP logging stubbed, crypto backends NOT compiled) and provide stub
 * pki_ecdsa_p256_verify / pki_ed25519_verify that capture the canonical
 * bytes the device would sign/verify. The captured bytes are memcmp'd
 * against vectors.h, which gen_vectors.py built with the REAL Python
 * canonical-byte implementation from tools/bake_tier2_keys.py and signed
 * with ephemeral keys via the `cryptography` package.
 *
 * What this proves: the C canonicalization (provision bytes, canonical
 * JSON, rotate bytes) is byte-identical to the Python/Lambda side, and
 * the accept/reject logic follows the brief. What it does NOT prove:
 * the mbedTLS ECDSA and Monocypher Ed25519 primitives themselves —
 * those run on-device (idf.py build compiles them; bench validation
 * signs real commands with the Lambda).
 */
#define PKI_HOST_TEST 1

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "vectors.h"
#include "../pki_verify.c" /* pulls pki_verify.h, tier2_keys.h, cJSON.h */

/* ---- stub crypto backends: capture canonical bytes, return canned rc -- */
static uint8_t g_cap[2048];
static size_t  g_cap_len;
static int     g_ecdsa_ret;
static int     g_ed_ret;

int pki_ecdsa_p256_verify(const uint8_t p[65], const uint8_t *m,
                          size_t ml, const uint8_t s[64])
{
    (void)p;
    (void)s;
    if (ml > sizeof(g_cap)) {
        ml = sizeof(g_cap);
    }
    memcpy(g_cap, m, ml);
    g_cap_len = ml;
    return g_ecdsa_ret;
}

int pki_ed25519_verify(const uint8_t p[32], const uint8_t *m,
                       size_t ml, const uint8_t s[64])
{
    (void)p;
    (void)s;
    if (ml > sizeof(g_cap)) {
        ml = sizeof(g_cap);
    }
    memcpy(g_cap, m, ml);
    g_cap_len = ml;
    return g_ed_ret;
}

/* ---- tiny framework ---- */
static int g_fails;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        if (!(cond)) {                                              \
            printf("FAIL line %d: ", __LINE__);                      \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                           \
            g_fails++;                                              \
        }                                                           \
    } while (0)

static void test_iso8601(void)
{
    uint64_t u;
    printf("  iso8601...\n");
    CHECK(pki_parse_iso8601_utc("2026-09-27T00:00:00Z", &u) == PKI_OK &&
          u == VEC_UNIX_20260927, "valid timestamp");
    CHECK(pki_parse_iso8601_utc("2024-02-29T12:34:56Z", &u) == PKI_OK,
          "leap day ok");
    CHECK(pki_parse_iso8601_utc("2023-02-29T12:00:00Z", &u) ==
          PKI_ERR_SCHEMA, "non-leap Feb 29 rejected");
    CHECK(pki_parse_iso8601_utc("2026-13-01T00:00:00Z", &u) ==
          PKI_ERR_SCHEMA, "month 13 rejected");
    CHECK(pki_parse_iso8601_utc("2026-09-27 00:00:00Z", &u) ==
          PKI_ERR_SCHEMA, "space separator rejected");
    CHECK(pki_parse_iso8601_utc("2026-09-27T00:00:00", &u) ==
          PKI_ERR_SCHEMA, "missing Z rejected");
    CHECK(pki_parse_iso8601_utc("2026-09-27T24:00:00Z", &u) ==
          PKI_ERR_SCHEMA, "hour 24 rejected");
    CHECK(pki_parse_iso8601_utc("2019-01-01T00:00:00Z", &u) ==
          PKI_ERR_SCHEMA, "year < 2020 rejected");
    CHECK(pki_parse_iso8601_utc(NULL, &u) == PKI_ERR_ARG, "null arg");
    CHECK(pki_parse_iso8601_utc("2026-09-27T00:00:60Z", &u) == PKI_OK &&
          u == VEC_UNIX_20260927 + 59, "leap second clamped");
}

static void test_hex(void)
{
    uint8_t b[4];
    printf("  hex...\n");
    CHECK(pki_hex_to_bytes("deadBEEF", b, 4) == 4 &&
          b[0] == 0xde && b[1] == 0xad && b[2] == 0xbe && b[3] == 0xef,
          "mixed case decode");
    CHECK(pki_hex_to_bytes("abc", b, 4) == 0, "length mismatch -> 0");
    CHECK(pki_hex_to_bytes("zzzzzzzz", b, 4) == 0, "bad char -> 0");
    CHECK(pki_hex_to_bytes("deadbee", b, 4) == 0, "odd length -> 0");
    CHECK(pki_hex_to_bytes(NULL, b, 4) == 0, "null -> 0");
}

static const char *json_in(int i)
{
    switch (i) {
    case 0:  return VEC_JSON_IN_0;
    case 1:  return VEC_JSON_IN_1;
    case 2:  return VEC_JSON_IN_2;
    case 3:  return VEC_JSON_IN_3;
    case 4:  return VEC_JSON_IN_4;
    case 5:  return VEC_JSON_IN_5;
    case 6:  return VEC_JSON_IN_6;
    case 7:  return VEC_JSON_IN_7;
    case 8:  return VEC_JSON_IN_8;
    case 9:  return VEC_JSON_IN_9;
    case 10: return VEC_JSON_IN_10;
    case 11: return VEC_JSON_IN_11;
    default: return NULL;
    }
}

static const char *json_exp(int i)
{
    switch (i) {
    case 0:  return VEC_JSON_EXP_0;
    case 1:  return VEC_JSON_EXP_1;
    case 2:  return VEC_JSON_EXP_2;
    case 3:  return VEC_JSON_EXP_3;
    case 4:  return VEC_JSON_EXP_4;
    case 5:  return VEC_JSON_EXP_5;
    case 6:  return VEC_JSON_EXP_6;
    case 7:  return VEC_JSON_EXP_7;
    case 8:  return VEC_JSON_EXP_8;
    case 9:  return VEC_JSON_EXP_9;
    case 10: return VEC_JSON_EXP_10;
    case 11: return VEC_JSON_EXP_11;
    default: return NULL;
    }
}

static void test_canon_json(void)
{
    int i;
    printf("  canon_json (%d cases vs CPython)...\n", VEC_JSON_NCASES);
    for (i = 0; i < VEC_JSON_NCASES; i++) {
        cJSON *n = cJSON_Parse(json_in(i));
        char out[1024];
        size_t olen = 0;
        pki_rc_t rc;
        CHECK(n != NULL, "case %d parses", i);
        if (!n) {
            continue;
        }
        rc = pki_canon_json(n, out, sizeof(out), &olen);
        CHECK(rc == PKI_OK, "case %d rc=%s", i, pki_rc_str(rc));
        if (rc == PKI_OK) {
            CHECK(strcmp(out, json_exp(i)) == 0,
                  "case %d mismatch:\n got: %s\n exp: %s",
                  i, out, json_exp(i));
        }
        cJSON_Delete(n);
    }
    /* error paths */
    {
        cJSON *n = cJSON_Parse("{\"f\":1.5}");
        char out[64];
        CHECK(pki_canon_json(n, out, sizeof(out), NULL) ==
              PKI_ERR_SCHEMA, "float rejected");
        cJSON_Delete(n);
    }
    {
        cJSON *n = cJSON_Parse("{\"a\":1}");
        char out[4];
        CHECK(pki_canon_json(n, out, sizeof(out), NULL) ==
              PKI_ERR_NOMEM, "tiny buffer -> NOMEM");
        cJSON_Delete(n);
    }
}

static void test_provision_canon(void)
{
    cJSON *root = cJSON_Parse(VEC_CMD_JSON);
    const cJSON *params;
    char pc[1024];
    uint8_t nonce[16], canon[1536];
    size_t clen = 0;
    pki_rc_t rc;
    printf("  provision_canon (C vs Python bytes)...\n");
    CHECK(root != NULL, "vector parses");
    params = cJSON_GetObjectItem(root, "params");
    CHECK(pki_canon_json(params, pc, sizeof(pc), NULL) == PKI_OK,
          "params canon ok");
    CHECK(strcmp(pc, VEC_PARAMS_CANON) == 0, "params canon matches");
    CHECK(pki_hex_to_bytes("0123456789abcdef0123456789abcdef",
                           nonce, 16) == 16, "nonce decodes");
    rc = pki_build_provision_canon("install_commander_key", "*",
                                   nonce, VEC_UNIX_20260927,
                                   VEC_UNIX_20260927 + 900, pc,
                                   canon, sizeof(canon), &clen);
    CHECK(rc == PKI_OK, "canon build ok");
    CHECK(clen == sizeof(VEC_CANON) &&
          memcmp(canon, VEC_CANON, clen) == 0,
          "canon bytes byte-identical to Python");
    cJSON_Delete(root);
}

/* Build a mutated command: copy VEC_CMD_JSON, swap in a fresh nonce,
 * apply one textual patch (lengths may differ), run pki_verify_provision. */
static pki_rc_t run_mutated(const char *nonce_hex, const char *from,
                            const char *to, bool keyed, int ecdsa_ret)
{
    static char buf[4096];
    char *p;
    pki_provision_cmd_t out;
    strcpy(buf, VEC_CMD_JSON);
    p = strstr(buf, "0123456789abcdef0123456789abcdef");
    if (p) {
        memcpy(p, nonce_hex, 32);
    }
    if (from && to) {
        p = strstr(buf, from);
        if (p) {
            size_t fl = strlen(from), tl = strlen(to);
            memmove(p + tl, p + fl, strlen(p + fl) + 1);
            memcpy(p, to, tl);
        }
    }
    pki_set_device_context("watcher-test", keyed);
    g_ecdsa_ret = ecdsa_ret;
    g_cap_len = 0;
    return pki_verify_provision((const uint8_t *)buf, strlen(buf),
                                VEC_SIG_RAW64, &out);
}

static void test_provision_accept(void)
{
    pki_provision_cmd_t out;
    pki_rc_t rc;
    uint8_t expect_nonce[16];
    printf("  provision accept + replay...\n");
    pki_set_device_context("watcher-test", false);
    g_ecdsa_ret = 0;
    g_cap_len = 0;
    rc = pki_verify_provision((const uint8_t *)VEC_CMD_JSON,
                              strlen(VEC_CMD_JSON),
                              VEC_SIG_RAW64, &out);
    CHECK(rc == PKI_OK, "accept rc=%s", pki_rc_str(rc));
    CHECK(g_cap_len == sizeof(VEC_CANON) &&
          memcmp(g_cap, VEC_CANON, g_cap_len) == 0,
          "signed bytes == Python canonical bytes");
    CHECK(out.kind == PKI_CMD_INSTALL_COMMANDER_KEY, "kind parsed");
    CHECK(strcmp(out.cmd, "install_commander_key") == 0, "cmd parsed");
    CHECK(strcmp(out.device, "*") == 0, "device parsed");
    CHECK(out.issued_at == VEC_UNIX_20260927, "issued parsed");
    CHECK(out.expires_at == VEC_UNIX_20260927 + 900, "expires parsed");
    CHECK(strcmp(out.params_json, VEC_PARAMS_CANON) == 0,
          "params_json is the signed canonical form");
    pki_hex_to_bytes("0123456789abcdef0123456789abcdef",
                     expect_nonce, 16);
    CHECK(memcmp(out.nonce, expect_nonce, 16) == 0, "nonce parsed");
    /* replay of the exact same command must fail */
    rc = pki_verify_provision((const uint8_t *)VEC_CMD_JSON,
                              strlen(VEC_CMD_JSON),
                              VEC_SIG_RAW64, &out);
    CHECK(rc == PKI_ERR_NONCE, "replay rejected (rc=%s)",
          pki_rc_str(rc));
}

static void test_provision_errors(void)
{
    pki_rc_t rc;
    printf("  provision error paths...\n");
    rc = run_mutated("11111111111111111111111111111111",
                     "\"watcher-provision-v1\"", "\"watcher-provision-v2\"",
                     false, 0);
    CHECK(rc == PKI_ERR_SCHEMA, "bad format -> SCHEMA (got %s)",
          pki_rc_str(rc));
    rc = run_mutated("22222222222222222222222222222222",
                     "\"install_commander_key\"",
                     "\"reboot_everything\"",
                     false, 0);
    CHECK(rc == PKI_ERR_SCHEMA, "unknown cmd -> SCHEMA (got %s)",
          pki_rc_str(rc));
    rc = run_mutated("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
                     NULL, NULL, false, 0);
    CHECK(rc == PKI_ERR_SCHEMA, "bad nonce hex -> SCHEMA (got %s)",
          pki_rc_str(rc));
    rc = run_mutated("33333333333333333333333333333333",
                     "\"issued_at\":\"2026-09-27T00:00:00Z\"",
                     "\"issued_at\":\"2026-09-27T01:00:00Z\"",
                     false, 0);
    CHECK(rc == PKI_ERR_WINDOW, "issued>expires -> WINDOW (got %s)",
          pki_rc_str(rc));
    rc = run_mutated("44444444444444444444444444444444",
                     "\"2026-09-27T00:15:00Z\"", "\"2026-09-27T02:15:00Z\"",
                     false, 0);
    CHECK(rc == PKI_ERR_WINDOW, "2h window -> WINDOW (got %s)",
          pki_rc_str(rc));
    rc = run_mutated("55555555555555555555555555555555",
                     "\"device\":\"*\"", "\"device\":\"nope\"",
                     false, 0);
    CHECK(rc == PKI_ERR_SCOPE, "device mismatch -> SCOPE (got %s)",
          pki_rc_str(rc));
    rc = run_mutated("66666666666666666666666666666666",
                     NULL, NULL, true, 0);
    CHECK(rc == PKI_ERR_SCOPE, "wildcard while keyed -> SCOPE (got %s)",
          pki_rc_str(rc));
    {
        /* pubkey_hex 62 chars instead of 64 -> schema violation */
        static char buf[4096];
        pki_provision_cmd_t out;
        char *p;
        strcpy(buf, VEC_CMD_JSON);
        p = strstr(buf, "0123456789abcdef0123456789abcdef");
        memcpy(p, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32);
        p = strstr(buf, "\"pubkey_hex\":\"");
        memmove(p + 14, p + 16, strlen(p + 16) + 1); /* drop 2 hex */
        pki_set_device_context("watcher-test", false);
        g_ecdsa_ret = 0;
        rc = pki_verify_provision((const uint8_t *)buf, strlen(buf),
                                  VEC_SIG_RAW64, &out);
        CHECK(rc == PKI_ERR_SCHEMA, "short pubkey -> SCHEMA (got %s)",
              pki_rc_str(rc));
    }
    rc = run_mutated("88888888888888888888888888888888",
                     NULL, NULL, false, -1);
    CHECK(rc == PKI_ERR_BADSIG, "bad sig -> BADSIG (got %s)",
          pki_rc_str(rc));
    {
        /* targeted device id must match */
        static char buf[4096];
        pki_provision_cmd_t out;
        const char *df = "\"device\":\"*\"";
        const char *dt = "\"device\":\"watcher-test\"";
        char *p;
        strcpy(buf, VEC_CMD_JSON);
        p = strstr(buf, "0123456789abcdef0123456789abcdef");
        memcpy(p, "99999999999999999999999999999999", 32);
        p = strstr(buf, df);
        memmove(p + strlen(dt), p + strlen(df),
                strlen(p + strlen(df)) + 1);
        memcpy(p, dt, strlen(dt));
        pki_set_device_context("watcher-test", true);
        g_ecdsa_ret = 0;
        rc = pki_verify_provision((const uint8_t *)buf, strlen(buf),
                                  VEC_SIG_RAW64, &out);
        CHECK(rc == PKI_OK, "targeted device accepted (got %s)",
              pki_rc_str(rc));
    }
    {
        /* truncated JSON */
        pki_provision_cmd_t out;
        pki_set_device_context("watcher-test", false);
        rc = pki_verify_provision((const uint8_t *)VEC_CMD_JSON, 40,
                                  VEC_SIG_RAW64, &out);
        CHECK(rc == PKI_ERR_PARSE, "truncated -> PARSE (got %s)",
              pki_rc_str(rc));
    }
}

static void test_rotate(void)
{
    pki_rc_t rc;
    printf("  rotate_tier1...\n");
    g_ed_ret = 0;
    g_cap_len = 0;
    rc = pki_verify_rotate_tier1(VEC_ROTATE_PUB65, VEC_ROTATE_EXP,
                                 VEC_ROTATE_SERIAL, VEC_ROTATE_SIG64);
    CHECK(rc == PKI_OK, "rotate accept (got %s)", pki_rc_str(rc));
    CHECK(g_cap_len == sizeof(VEC_ROTATE_CANON) &&
          memcmp(g_cap, VEC_ROTATE_CANON, g_cap_len) == 0,
          "rotate canon bytes byte-identical to Python");
    rc = pki_verify_rotate_tier1(VEC_ROTATE_PUB65, VEC_ROTATE_EXP,
                                 VEC_BAKED_SERIAL, VEC_ROTATE_SIG64);
    CHECK(rc == PKI_ERR_SCHEMA, "serial not monotonic -> SCHEMA");
    rc = pki_verify_rotate_tier1(VEC_ROTATE_PUB65, VEC_BAKED_EXP,
                                 VEC_ROTATE_SERIAL, VEC_ROTATE_SIG64);
    CHECK(rc == PKI_ERR_SCHEMA, "expiry not monotonic -> SCHEMA");
    rc = pki_verify_rotate_tier1(VEC_ROTATE_PUB65, VEC_ROTATE_EXP,
                                 0, VEC_ROTATE_SIG64);
    CHECK(rc == PKI_ERR_SCHEMA, "serial 0 -> SCHEMA");
    g_ed_ret = -1;
    rc = pki_verify_rotate_tier1(VEC_ROTATE_PUB65, VEC_ROTATE_EXP,
                                 VEC_ROTATE_SERIAL, VEC_ROTATE_SIG64);
    CHECK(rc == PKI_ERR_BADSIG, "bad Tier0 sig -> BADSIG");
    g_ed_ret = 0;
}

/* Build a mutated rotate_tier1 provision command: copy
 * VEC_ROTATE_CMD_JSON, apply one textual patch (lengths may differ),
 * run pki_verify_provision with the matching outer signature. */
static pki_rc_t run_rot_mutated(const char *nonce_hex, const char *from,
                                const char *to, int ed_ret,
                                pki_provision_cmd_t *out)
{
    static char buf[4096];
    char *p;
    pki_provision_cmd_t dummy;
    strcpy(buf, VEC_ROTATE_CMD_JSON);
    p = strstr(buf, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    if (p) {
        memcpy(p, nonce_hex, 32);
    }
    if (from && to) {
        p = strstr(buf, from);
        if (p) {
            size_t fl = strlen(from), tl = strlen(to);
            memmove(p + tl, p + fl, strlen(p + fl) + 1);
            memcpy(p, to, tl);
        }
    }
    pki_set_device_context("watcher-test", false); /* wildcard: unkeyed */
    g_ecdsa_ret = 0;   /* outer Tier 1 signature: valid */
    g_ed_ret = ed_ret; /* inner Tier 0 countersignature stub */
    g_cap_len = 0;
    return pki_verify_provision((const uint8_t *)buf, strlen(buf),
                                VEC_ROTATE_CMD_SIG64,
                                out ? out : &dummy);
}

static void test_rotate_provision(void)
{
    pki_rc_t rc;
    pki_provision_cmd_t out;
    char from[320], to[320];
    printf("  rotate_tier1 provision (Tier 0 countersignature)...\n");

    rc = run_rot_mutated("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                         NULL, NULL, 0, &out);
    CHECK(rc == PKI_OK, "rotate provision accept (got %s)",
          pki_rc_str(rc));
    CHECK(out.kind == PKI_CMD_ROTATE_TIER1, "kind == rotate_tier1");

    /* missing Tier 0 countersignature -> rejected */
    snprintf(from, sizeof(from), "\"tier0_sig_hex\":\"%s\",",
             VEC_ROTATE_TIER0_SIG_HEX);
    rc = run_rot_mutated("cccccccccccccccccccccccccccccccc",
                         from, "", 0, NULL);
    CHECK(rc == PKI_ERR_SCHEMA,
          "rotate w/o Tier0 sig -> SCHEMA (got %s)", pki_rc_str(rc));

    /* missing serial -> rejected */
    snprintf(from, sizeof(from), ",\"serial\":%u", VEC_ROTATE_SERIAL);
    rc = run_rot_mutated("dddddddddddddddddddddddddddddddd",
                         from, "", 0, NULL);
    CHECK(rc == PKI_ERR_SCHEMA,
          "rotate w/o serial -> SCHEMA (got %s)", pki_rc_str(rc));

    /* corrupted Tier 0 countersignature -> BADSIG */
    snprintf(from, sizeof(from), "\"tier0_sig_hex\":\"%s\"",
             VEC_ROTATE_TIER0_SIG_HEX);
    snprintf(to, sizeof(to), "\"tier0_sig_hex\":\"%s\"",
             VEC_ROTATE_TIER0_SIG_HEX);
    to[20] = (to[20] == 'a') ? 'b' : 'a'; /* flip one hex char */
    rc = run_rot_mutated("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
                         from, to, -1, NULL);
    CHECK(rc == PKI_ERR_BADSIG,
          "rotate bad Tier0 sig -> BADSIG (got %s)", pki_rc_str(rc));
}

static void test_nonce_store(void)
{
    uint8_t n[16];
    int i;
    printf("  nonce store...\n");
    memset(n, 0x5a, 16);
    /* fresh store: pki_verify_provision tests above recorded nonces;
     * use a distinct one here */
    CHECK(!pki_nonce_seen(n), "unseen nonce");
    CHECK(pki_nonce_record(n) == PKI_OK, "record ok");
    CHECK(pki_nonce_seen(n), "seen after record");
    CHECK(pki_nonce_record(n) == PKI_ERR_NONCE, "double record");
    for (i = 0; i < 70; i++) {
        memset(n, (uint8_t)i, 16);
        n[0] ^= 0xa5;
        pki_nonce_record(n);
    }
    CHECK(pki_nonce_seen(n), "post-eviction tail present");
}

int main(void)
{
    printf("pki host test\n");
    test_iso8601();
    test_hex();
    test_canon_json();
    test_provision_canon();
    test_provision_accept();
    test_provision_errors();
    test_rotate();
    test_rotate_provision();
    test_nonce_store();
    if (g_fails == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", g_fails);
    return 1;
}
