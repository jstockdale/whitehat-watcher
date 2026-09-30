/* SPDX-License-Identifier: MIT
 * Host test harness for components/seal (Phase 2 WMS2 verifier).
 *
 * Covers:
 *   - RFC 8032 section 7 test vectors (tests 1, 2, 3) against
 *     crypto_ed25519_check, plus key_pair determinism and
 *     sign-against-vector checks.
 *   - Monocypher <-> PyNaCl interop, both directions, including
 *     byte-identical deterministic signatures.
 *   - Cross-verification vs mqtt_seal.py, both directions,
 *     kind=2 (control) and kind=1 (telemetry).
 *   - Every named rejection and the grace/strict policy gates.
 *
 * Python interop shells out to: python3 -c '<dispatcher>' with the
 * mqtt-bridge directory on sys.path. It defaults to
 *   $HOME/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge
 * or set WMS2_MQTT_BRIDGE to override.
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -Werror -Wno-comment -O2 -I.. -I../include \
 *       test_wms2.c ../wms2.c ../monocypher.c ../optional/monocypher-ed25519.c \
 *       -o test_wms2
 * Run:
 *   ./test_wms2            (exit 0 on ALL PASS, 1 otherwise)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wms2.h"
#include "monocypher.h"
#include "optional/monocypher-ed25519.h"

/* ------------------------------------------------------------------ */
/* test bookkeeping                                                    */
/* ------------------------------------------------------------------ */
static int n_pass = 0, n_fail = 0;

static void check(const char *name, int cond)
{
    if (cond) {
        n_pass++;
        printf("PASS  %s\n", name);
    } else {
        n_fail++;
        printf("FAIL  %s\n", name);
    }
}

/* ------------------------------------------------------------------ */
/* hex helpers                                                         */
/* ------------------------------------------------------------------ */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* returns bytes written, or -1 on bad input */
static int hexdec(uint8_t *out, size_t cap, const char *hex)
{
    size_t n = strlen(hex), i;
    int hi, lo;
    if (n % 2) return -1;
    if (n / 2 > cap) return -1;
    for (i = 0; i < n; i += 2) {
        hi = hexval(hex[i]);
        lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

static void hexenc(char *out, const uint8_t *in, size_t len)
{
    static const char d[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < len; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * len] = 0;
}

/* ------------------------------------------------------------------ */
/* python dispatcher (single -c program, double quotes only inside)    */
/* ------------------------------------------------------------------ */
static const char PYDISPATCH[] =
"import sys\n"
"def B(x):\n"
"    return bytes.fromhex(x)\n"
"bridge=sys.argv[1]\n"
"cmd=sys.argv[2]\n"
"a=sys.argv[3:]\n"
"sys.path.insert(0,bridge)\n"
"from mqtt_seal import seal_control,seal_telemetry,verify,Policy,ReplayCtx\n"
"from nacl.signing import SigningKey,VerifyKey\n"
"if cmd==\"seal_ctl\":\n"
"    seed,topic,dev,ctr,ts,body=a\n"
"    sk=SigningKey(B(seed))\n"
"    env=seal_control(topic,dev,int(ctr),int(ts),B(body),sk)\n"
"    print(env.hex())\n"
"elif cmd==\"verify_ctl\":\n"
"    envh,vkh,topic,now,hw,strict,win=a\n"
"    rp=ReplayCtx(int(hw))\n"
"    pol=Policy(strict=bool(int(strict)),ts_window_ms=int(win))\n"
"    rc,h,body,log=verify(B(envh),topic,verify_key=B(vkh),now_ms=int(now),replay=rp,policy=pol)\n"
"    print(rc)\n"
"    print(rp.high_water)\n"
"    print(body.hex() if body is not None else \"-\")\n"
"    print(\"-\" if log is None else log.replace(chr(10),\" \"))\n"
"elif cmd==\"seal_tel\":\n"
"    key,topic,dev,ctr,ts,body=a\n"
"    env=seal_telemetry(topic,dev,int(ctr),int(ts),B(body),B(key))\n"
"    print(env.hex())\n"
"elif cmd==\"verify_tel\":\n"
"    envh,keyh,topic,now,hw,strict,win=a\n"
"    rp=ReplayCtx(int(hw))\n"
"    pol=Policy(strict=bool(int(strict)),ts_window_ms=int(win))\n"
"    rc,h,body,log=verify(B(envh),topic,key32=B(keyh),now_ms=int(now),replay=rp,policy=pol)\n"
"    print(rc)\n"
"    print(rp.high_water)\n"
"    print(body.hex() if body is not None else \"-\")\n"
"    print(\"-\" if log is None else log.replace(chr(10),\" \"))\n"
"elif cmd==\"nacl_verify\":\n"
"    pkh,sigh,msgh=a\n"
"    VerifyKey(B(pkh)).verify(B(sigh)+B(msgh))\n"
"    print(\"OK\")\n"
"elif cmd==\"nacl_sign\":\n"
"    seedh,msgh=a\n"
"    sk=SigningKey(B(seedh))\n"
"    sm=sk.sign(B(msgh))\n"
"    print(sk.verify_key.encode().hex())\n"
"    print(sm.signature.hex())\n"
"else:\n"
"    print(\"BADCMD\")\n";

static const char *bridge_dir(void)
{
    static char buf[2048];
    const char *e = getenv("WMS2_MQTT_BRIDGE");
    const char *home;
    if (e && *e) return e;
    home = getenv("HOME");
    if (!home) home = "";
    snprintf(buf, sizeof buf,
             "%s/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge",
             home);
    return buf;
}

/* run the dispatcher; capture stdout into out. returns 0 on success. */
static int pyrun(char *out, size_t outcap, const char *cmd,
                 int argc, const char *const *argv)
{
    char shell[16384];
    size_t pos = 0;
    int n, i;
    size_t got;
    FILE *fp;

    n = snprintf(shell + pos, sizeof shell - pos,
                 "python3 -c '%s' '%s' '%s'",
                 PYDISPATCH, bridge_dir(), cmd);
    if (n < 0 || (size_t)n >= sizeof shell - pos) return -1;
    pos += (size_t)n;
    for (i = 0; i < argc; i++) {
        n = snprintf(shell + pos, sizeof shell - pos, " '%s'", argv[i]);
        if (n < 0 || (size_t)n >= sizeof shell - pos) return -1;
        pos += (size_t)n;
    }
    fp = popen(shell, "r");
    if (!fp) return -1;
    got = fread(out, 1, outcap - 1, fp);
    out[got] = 0;
    n = pclose(fp);
    return (n == 0 && got > 0) ? 0 : -1;
}

/* split captured output into NUL-terminated lines; returns count */
static int pylines(char *out, char **lines, int maxlines)
{
    int n = 0;
    char *p = out, *q;
    while (n < maxlines && *p) {
        lines[n++] = p;
        q = strchr(p, '\n');
        if (!q) break;
        *q = 0;
        p = q + 1;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* test-local kind=1 sealer (mirrors wms2 auth semantics; the shipped  */
/* header only declares wms2_seal_control, so kind=1 sealing lives in  */
/* the harness for cross-verification)                                 */
/* ------------------------------------------------------------------ */
static int seal_telemetry_local(uint8_t *out, size_t cap, const char *topic,
                                const char *dev_id, uint8_t flags,
                                uint64_t ctr, uint64_t ts_ms,
                                const uint8_t *body, size_t body_len,
                                const uint8_t key[32])
{
    size_t tlen = strlen(topic), dl = strlen(dev_id), i;
    uint8_t tag[WMS2_TAG_LEN], tl[2];
    crypto_blake2b_ctx c;
    if (dl > WMS2_DEVID_LEN || tlen > 65535) return -1;
    if (cap < WMS2_HDR_LEN + body_len + WMS2_TAG_LEN) return -1;
    memcpy(out, WMS2_MAGIC, 4);
    out[4] = WMS2_KIND_TELEM;
    out[5] = flags;
    memset(out + 6, 0, WMS2_DEVID_LEN);
    memcpy(out + 6, dev_id, dl);
    for (i = 0; i < 8; i++) {
        out[22 + i] = (uint8_t)(ctr >> (8 * i));
        out[30 + i] = (uint8_t)(ts_ms >> (8 * i));
    }
    out[38] = out[39] = 0;
    memcpy(out + WMS2_HDR_LEN, body, body_len);
    crypto_blake2b_keyed_init(&c, WMS2_TAG_LEN, key, 32);
    tl[0] = (uint8_t)tlen;
    tl[1] = (uint8_t)(tlen >> 8);
    crypto_blake2b_update(&c, tl, 2);
    crypto_blake2b_update(&c, (const uint8_t *)topic, tlen);
    crypto_blake2b_update(&c, out, WMS2_HDR_LEN);
    crypto_blake2b_update(&c, body, body_len);
    crypto_blake2b_final(&c, tag);
    memcpy(out + WMS2_HDR_LEN + body_len, tag, WMS2_TAG_LEN);
    crypto_wipe(tag, sizeof tag);
    return (int)(WMS2_HDR_LEN + body_len + WMS2_TAG_LEN);
}

/* ------------------------------------------------------------------ */
/* RFC 8032 section 7 vectors (copied from the RFC text)                */
/* ------------------------------------------------------------------ */
struct rfcvec {
    const char *seed, *pk, *msg, *sig;
};
static const struct rfcvec RFCV[] = {
    { /* TEST 1: empty message */
      "9d61b19deffd5a60ba844af492ec2cc4"
      "4449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a"
      "0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a"
      "84877f1eb8e5d974d873e06522490155"
      "5fb8821590a33bacc61e39701cf9b46b"
      "d25bf5f0595bbe24655141438e7a100b" },
    { /* TEST 2: one byte 0x72 */
      "4ccd089b28ff96da9db6c346ec114e0f"
      "5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc"
      "9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540"
      "a2b27b5416503f8fb3762223ebdb69da"
      "085ac1e43e15996e458f3613d0f11d8c"
      "387b2eaeb4302aeeb00d291612bb0c00" },
    { /* TEST 3: two bytes af82 */
      "c5aa8df43f9f837bedb7442f31dcb7b1"
      "66d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f058"
      "0816ed13ba3303ac5deb911548908025",
      "af82",
      "6291d657deec24024827e69c3abe01a3"
      "0ce548a284743a445e3680d7db5ac3ac"
      "18ff9b538d16f290ae67f760984dc659"
      "4a7c15e9716ed28dc027beceea1ec40a" },
};

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
#define SCRATCH_SZ 4096
#define ENV_SZ     4096

int main(void)
{
    /* fixed, deterministic test vectors -------------------------------- */
    static const char *TOPIC = "watcher/command";
    static const char *TTOPIC = "watcher/telemetry";
    static const char *DEV = "muse-01";
    static const char *BODY = "{\"t\":\"ping\"}";
    const uint64_t TS = 1758850000000ULL;
    const uint64_t NOW = TS + 1000;

    uint8_t seed[32], sk[64], pk[32], key32[32];
    uint8_t env[ENV_SZ], scratch[SCRATCH_SZ];
    uint8_t msgbuf[64];
    char hex[ENV_SZ * 2 + 1], hex2[256], hex3[256];
    char pyout[16384], *lines[8];
    wms2_replay rp;
    wms2_policy pol;
    wms2_hdr h;
    const uint8_t *bp;
    size_t bl;
    wms2_rc rc;
    size_t i, j;
    int n, elen;
    uint8_t sig[64], sig2[64], pk2[32], seedmut[32], tag[16];

    for (i = 0; i < 32; i++) { seed[i] = (uint8_t)i; key32[i] = (uint8_t)(0xa0 + i); }

    /* the python side must be reachable */
    {
        char probe[4096];
        snprintf(probe, sizeof probe, "%s/mqtt_seal.py", bridge_dir());
        FILE *f = fopen(probe, "r");
        check("mqtt_seal.py reachable", f != NULL);
        if (!f) {
            printf("\n1 FAILURE (python bridge missing)\n");
            return 1;
        }
        fclose(f);
    }

    /* ---- RFC 8032 vectors ------------------------------------------ */
    for (i = 0; i < sizeof RFCV / sizeof RFCV[0]; i++) {
        uint8_t rseed[32], rpk[32], rsig[64], rsk[64];
        uint8_t rmsg[1024], mysig[64];
        char name[64];
        int rml = hexdec(rseed, sizeof rseed, RFCV[i].seed);
        int rpl = hexdec(rpk, sizeof rpk, RFCV[i].pk);
        int rsl = hexdec(rsig, sizeof rsig, RFCV[i].sig);
        int mlen2 = hexdec(rmsg, sizeof rmsg, RFCV[i].msg);
        snprintf(name, sizeof name, "rfc8032 test%u vector parses", (unsigned)(i + 1));
        check(name, rml == 32 && rpl == 32 && rsl == 64 && mlen2 >= 0);
        if (mlen2 < 0) mlen2 = 0;
        snprintf(name, sizeof name, "rfc8032 test%u check() accepts", (unsigned)(i + 1));
        check(name, crypto_ed25519_check(rsig, rpk, rmsg, (size_t)mlen2) == 0);
        /* tampered vector must fail */
        rsig[0] ^= 1;
        snprintf(name, sizeof name, "rfc8032 test%u check() rejects tampered sig",
                 (unsigned)(i + 1));
        check(name, crypto_ed25519_check(rsig, rpk, rmsg, (size_t)mlen2) != 0);
        rsig[0] ^= 1;
        /* key_pair from the RFC seed reproduces the RFC public key */
        crypto_ed25519_key_pair(rsk, rpk, rseed);
        {
            uint8_t rpk2[32];
            hexdec(rpk2, sizeof rpk2, RFCV[i].pk);
            snprintf(name, sizeof name, "rfc8032 test%u key_pair() -> RFC pk",
                     (unsigned)(i + 1));
            check(name, crypto_verify32(rpk, rpk2) == 0);
        }
        /* deterministic sign() reproduces the RFC signature */
        crypto_ed25519_sign(mysig, rsk, rmsg, (size_t)mlen2);
        {
            uint8_t rsig2[64];
            hexdec(rsig2, sizeof rsig2, RFCV[i].sig);
            snprintf(name, sizeof name, "rfc8032 test%u sign() -> RFC sig",
                     (unsigned)(i + 1));
            check(name, memcmp(mysig, rsig2, 64) == 0);
        }
        crypto_wipe(rsk, sizeof rsk);
    }

    /* ---- Monocypher <-> PyNaCl interop ------------------------------- */
    memcpy(seedmut, seed, 32);
    crypto_ed25519_key_pair(sk, pk, seedmut);
    {
        const uint8_t *msg = (const uint8_t *)"interoperate, please";
        size_t msglen = strlen((const char *)msg);
        const char *av[3];
        hexenc(hex, seed, 32); av[0] = hex;
        hexenc(hex2, msg, msglen); av[1] = hex2;
        /* mono signs -> nacl verifies */
        crypto_ed25519_sign(sig, sk, msg, msglen);
        hexenc(hex, pk, 32);
        hexenc(hex2, sig, 64);
        hexenc(hex3, msg, msglen);
        av[0] = hex; av[1] = hex2; av[2] = hex3;
        n = pyrun(pyout, sizeof pyout, "nacl_verify", 3, av);
        check("monocypher sign -> PyNaCl verify", n == 0 &&
              strncmp(pyout, "OK", 2) == 0);
        /* nacl signs -> mono verifies, and signatures are byte-identical */
        hexenc(hex, seed, 32);
        hexenc(hex2, msg, msglen);
        av[0] = hex; av[1] = hex2;
        n = pyrun(pyout, sizeof pyout, "nacl_sign", 2, av);
        if (n == 0 && pylines(pyout, lines, 8) == 2 &&
            hexdec(pk2, sizeof pk2, lines[0]) == 32 &&
            hexdec(sig2, sizeof sig2, lines[1]) == 64) {
            check("PyNaCl sign -> monocypher check",
                  crypto_ed25519_check(sig2, pk2, msg, msglen) == 0);
            check("nacl pk == monocypher pk", crypto_verify32(pk2, pk) == 0);
            check("deterministic sigs byte-identical (mono==nacl)",
                  memcmp(sig, sig2, 64) == 0);
        } else {
            check("PyNaCl sign -> monocypher check", 0);
            check("nacl pk == monocypher pk", 0);
            check("deterministic sigs byte-identical (mono==nacl)", 0);
        }
    }

    /* ---- cross-verify vs mqtt_seal.py -------------------------------- */
    hexenc(hex, seed, 32);
    {
        const char *av[6];
        const uint8_t *body = (const uint8_t *)BODY;
        size_t bodylen = strlen(BODY);
        char ctrs[24], tss[24];
        snprintf(ctrs, sizeof ctrs, "%llu", 1ULL);
        snprintf(tss, sizeof tss, "%llu", (unsigned long long)TS);
        hexenc(hex2, body, bodylen);
        av[0] = hex; av[1] = TOPIC; av[2] = DEV;
        av[3] = ctrs; av[4] = tss; av[5] = hex2;
        /* python seals kind=2 -> C verifies */
        n = pyrun(pyout, sizeof pyout, "seal_ctl", 6, av);
        if (n == 0 && pylines(pyout, lines, 8) >= 1 &&
            hexdec(env, sizeof env, lines[0]) > 0) {
            elen = hexdec(env, sizeof env, lines[0]);
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.strict = true;
            pol.ts_window_ms = 120000;
            rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW,
                             &rp, &pol, &h, &bp, &bl);
            check("py seal_ctl -> C verify OK", rc == WMS2_OK);
            check("py->C body intact", bl == bodylen && memcmp(bp, body, bl) == 0);
            check("py->C header parsed",
                  h.kind == 2 && h.ctr == 1 && h.ts_ms == TS &&
                  strcmp(h.dev_id, DEV) == 0);
            check("py->C high-water adopted", rp.high_water == 1);
        } else {
            check("py seal_ctl -> C verify OK", 0);
            check("py->C body intact", 0);
            check("py->C header parsed", 0);
            check("py->C high-water adopted", 0);
        }
        /* C seals kind=2 -> python verifies */
        elen = wms2_seal_control(env, sizeof env, TOPIC, DEV, 2, TS,
                                 body, bodylen, sk);
        check("wms2_seal_control length",
              elen == (int)(WMS2_HDR_LEN + bodylen + WMS2_SIG_LEN));
        if (elen > 0) {
            char nows[24], bodyhex[512];
            const char *bv[7];
            hexenc(hex, env, (size_t)elen);
            hexenc(hex2, pk, 32);
            hexenc(bodyhex, body, bodylen);
            snprintf(nows, sizeof nows, "%llu", (unsigned long long)NOW);
            bv[0] = hex; bv[1] = hex2; bv[2] = TOPIC;
            bv[3] = nows; bv[4] = "0"; bv[5] = "1"; bv[6] = "120000";
            n = pyrun(pyout, sizeof pyout, "verify_ctl", 7, bv);
            if (n == 0 && pylines(pyout, lines, 8) == 4) {
                int prc = atoi(lines[0]);
                unsigned long long phw =
                    strtoull(lines[1], NULL, 10);
                check("C seal_ctl -> py verify OK", prc == 0 && phw == 2);
                check("C->py body intact", strcmp(lines[2], bodyhex) == 0);
                check("C->py silent (no log)", strcmp(lines[3], "-") == 0);
            } else {
                check("C seal_ctl -> py verify OK", 0);
                check("C->py body intact", 0);
                check("C->py silent (no log)", 0);
            }
        }
    }

    /* kind=1 both directions */
    {
        const uint8_t *tbody = (const uint8_t *)"{\"snap\":1}";
        size_t tbodylen = strlen((const char *)tbody);
        const char *av[6];
        char ctrs[24], tss[24], nows[24];
        hexenc(hex, key32, 32);
        hexenc(hex2, tbody, tbodylen);
        snprintf(ctrs, sizeof ctrs, "%llu", 1ULL);
        snprintf(tss, sizeof tss, "%llu", (unsigned long long)TS);
        av[0] = hex; av[1] = TTOPIC; av[2] = "watcher-01";
        av[3] = ctrs; av[4] = tss; av[5] = hex2;
        /* python seals kind=1 -> C verifies */
        n = pyrun(pyout, sizeof pyout, "seal_tel", 6, av);
        if (n == 0 && pylines(pyout, lines, 8) >= 1 &&
            (elen = hexdec(env, sizeof env, lines[0])) > 0) {
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.strict = true;
            pol.ts_window_ms = 120000;
            rc = wms2_verify(env, (size_t)elen, TTOPIC, key32, NULL,
                             scratch, sizeof scratch, NOW,
                             &rp, &pol, &h, &bp, &bl);
            check("py seal_tel -> C verify OK", rc == WMS2_OK);
            check("py->C kind1 body intact",
                  bl == tbodylen && memcmp(bp, tbody, bl) == 0);
            check("py->C kind1 header", h.kind == 1 && rp.high_water == 1);
        } else {
            check("py seal_tel -> C verify OK", 0);
            check("py->C kind1 body intact", 0);
            check("py->C kind1 header", 0);
        }
        /* C seals kind=1 (harness-local sealer) -> python verifies */
        elen = seal_telemetry_local(env, sizeof env, TTOPIC, "watcher-01", 0,
                                    3, TS, tbody, tbodylen, key32);
        check("local kind1 sealer length",
              elen == (int)(WMS2_HDR_LEN + tbodylen + WMS2_TAG_LEN));
        if (elen > 0) {
            const char *bv[7];
            char tbodyhex[512];
            hexenc(hex, env, (size_t)elen);
            hexenc(hex2, key32, 32);
            hexenc(tbodyhex, tbody, tbodylen);
            snprintf(nows, sizeof nows, "%llu", (unsigned long long)NOW);
            bv[0] = hex; bv[1] = hex2; bv[2] = TTOPIC;
            bv[3] = nows; bv[4] = "0"; bv[5] = "1"; bv[6] = "120000";
            n = pyrun(pyout, sizeof pyout, "verify_tel", 7, bv);
            if (n == 0 && pylines(pyout, lines, 8) == 4) {
                check("C seal_tel -> py verify OK", atoi(lines[0]) == 0);
                check("C->py kind1 body intact",
                      strcmp(lines[2], tbodyhex) == 0);
            } else {
                check("C seal_tel -> py verify OK", 0);
                check("C->py kind1 body intact", 0);
            }
        }
    }

    /* ---- every named rejection -------------------------------------- */
    {
        const uint8_t *body = (const uint8_t *)BODY;
        size_t bodylen = strlen(BODY);
        uint8_t bad[ENV_SZ];

        elen = wms2_seal_control(env, sizeof env, TOPIC, DEV, 1, TS,
                                 body, bodylen, sk);

        /* bad magic: strict drops, grace honors */
        memcpy(bad, env, (size_t)elen);
        memcpy(bad, "XXXX", 4);
        memset(&rp, 0, sizeof rp);
        memset(&pol, 0, sizeof pol);
        pol.strict = true; pol.ts_window_ms = 120000;
        rc = wms2_verify(bad, (size_t)elen, TOPIC, NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("bad magic strict -> UNSEALED", rc == WMS2_E_UNSEAL);
        memset(&pol, 0, sizeof pol);
        pol.ts_window_ms = 120000;
        rc = wms2_verify(bad, (size_t)elen, TOPIC, NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("bad magic grace -> OK-GRACE", rc == WMS2_OK_GRACE &&
              bl == (size_t)elen && bp == bad && h.kind == 0);
        check("bad magic never advances high-water", rp.high_water == 0);

        /* bad kind */
        memcpy(bad, env, (size_t)elen);
        bad[4] = 7;
        memset(&pol, 0, sizeof pol);
        pol.strict = true; pol.ts_window_ms = 120000;
        rc = wms2_verify(bad, (size_t)elen, TOPIC, NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("bad kind -> KIND", rc == WMS2_E_KIND);

        /* short: kind=2 needs 104, give 100 */
        memset(&pol, 0, sizeof pol);
        pol.strict = true; pol.ts_window_ms = 120000;
        rc = wms2_verify(env, 100, TOPIC, NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("short -> SHORT", rc == WMS2_E_SHORT);

        /* tampered body -> BAD-SIG, fatal even in grace */
        memcpy(bad, env, (size_t)elen);
        bad[WMS2_HDR_LEN + 2] ^= 0x01;
        memset(&rp, 0, sizeof rp);
        rp.high_water = 0;
        memset(&pol, 0, sizeof pol);      /* grace mode */
        pol.ts_window_ms = 120000;
        rc = wms2_verify(bad, (size_t)elen, TOPIC, NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("tampered body grace -> BAD-SIG (fatal)", rc == WMS2_E_BADSIG);
        check("BAD-SIG never advances high-water", rp.high_water == 0);

        /* cross-topic republish -> BAD-SIG (D1 topic binding) */
        memset(&rp, 0, sizeof rp);
        memset(&pol, 0, sizeof pol);
        pol.strict = true; pol.ts_window_ms = 120000;
        rc = wms2_verify(env, (size_t)elen, "watcher/telemetry", NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("cross-topic republish -> BAD-SIG (fatal)", rc == WMS2_E_BADSIG);
        check("cross-topic never advances high-water", rp.high_water == 0);

        /* dup ctr: silent OK-DUP */
        memset(&rp, 0, sizeof rp);
        memset(&pol, 0, sizeof pol);
        pol.strict = true; pol.ts_window_ms = 120000;
        rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("first ctr=1 -> OK", rc == WMS2_OK && rp.high_water == 1);
        rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, pk,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("dup ctr -> OK-DUP (silent)", rc == WMS2_OK_DUP);
        check("dup holds high-water", rp.high_water == 1);

        /* ctr rollback -> REPLAY (loud) */
        {
            uint8_t env5[ENV_SZ];
            int e5 = wms2_seal_control(env5, sizeof env5, TOPIC, DEV, 5, TS,
                                       body, bodylen, sk);
            memset(&rp, 0, sizeof rp);
            rc = wms2_verify(env5, (size_t)e5, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
            check("ctr=5 -> OK, hw=5", rc == WMS2_OK && rp.high_water == 5);
            rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
            check("ctr rollback -> REPLAY", rc == WMS2_E_REPLAY);
            check("REPLAY never advances high-water", rp.high_water == 5);
            /* forward motion still works after a dup */
            {
                uint8_t env6[ENV_SZ];
                int e6 = wms2_seal_control(env6, sizeof env6, TOPIC, DEV, 6, TS,
                                           body, bodylen, sk);
                rc = wms2_verify(env6, (size_t)e6, TOPIC, NULL, pk,
                                 scratch, sizeof scratch, NOW, &rp, &pol,
                                 &h, &bp, &bl);
                check("ctr=6 after replay -> OK", rc == WMS2_OK && rp.high_water == 6);
            }
        }

        /* stale ts -> STALE; ts_window=0 disables */
        {
            uint8_t old[ENV_SZ];
            int eo = wms2_seal_control(old, sizeof old, TOPIC, DEV, 9,
                                       TS - 600000, body, bodylen, sk);
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 120000;
            rc = wms2_verify(old, (size_t)eo, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("stale ts -> STALE", rc == WMS2_E_STALE);
            check("STALE never advances high-water", rp.high_water == 0);
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 0;   /* device: disabled */
            rc = wms2_verify(old, (size_t)eo, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("ts_window=0 accepts stale ts", rc == WMS2_OK &&
                  rp.high_water == 9);
        }

        /* no-key: grace vs strict, kind=2 */
        memset(&rp, 0, sizeof rp);
        memset(&pol, 0, sizeof pol);
        pol.ts_window_ms = 120000;
        rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, NULL,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("no pin grace -> OK-GRACE", rc == WMS2_OK_GRACE &&
              bl == bodylen && rp.high_water == 0);
        memset(&pol, 0, sizeof pol);
        pol.strict = true; pol.ts_window_ms = 120000;
        rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, NULL,
                         scratch, sizeof scratch, NOW, &rp, &pol, &h, &bp, &bl);
        check("no pin strict -> NO-KEY", rc == WMS2_E_NOKEY);

        /* unsealed input: grace vs strict */
        {
            const uint8_t *plain = (const uint8_t *)"{\"id\":\"x\"}";
            size_t plainlen = strlen((const char *)plain);
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.ts_window_ms = 120000;
            rc = wms2_verify(plain, plainlen, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("unsealed grace -> OK-GRACE", rc == WMS2_OK_GRACE &&
                  bp == plain && bl == plainlen && h.kind == 0);
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 120000;
            rc = wms2_verify(plain, plainlen, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("unsealed strict -> UNSEALED", rc == WMS2_E_UNSEAL);
        }

        /* kind=1: no key grace/strict, tampered tag */
        {
            const uint8_t *tbody = (const uint8_t *)"{\"snap\":1}";
            size_t tbodylen = strlen((const char *)tbody);
            uint8_t tenv[ENV_SZ], tbad[ENV_SZ];
            int te = seal_telemetry_local(tenv, sizeof tenv, TTOPIC,
                                          "watcher-01", 0, 1, TS,
                                          tbody, tbodylen, key32);
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.ts_window_ms = 120000;
            rc = wms2_verify(tenv, (size_t)te, TTOPIC, NULL, NULL,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("kind1 no key grace -> OK-GRACE", rc == WMS2_OK_GRACE);
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 120000;
            rc = wms2_verify(tenv, (size_t)te, TTOPIC, NULL, NULL,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("kind1 no key strict -> NO-KEY", rc == WMS2_E_NOKEY);
            memcpy(tbad, tenv, (size_t)te);
            tbad[te - 1] ^= 0x01;
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.ts_window_ms = 120000;
            rc = wms2_verify(tbad, (size_t)te, TTOPIC, key32, NULL,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("kind1 tampered tag grace -> BAD-TAG (fatal)",
                  rc == WMS2_E_BADTAG && rp.high_water == 0);
            /* cross-topic on kind=1 */
            rc = wms2_verify(tenv, (size_t)te, TOPIC, key32, NULL,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("kind1 cross-topic -> BAD-TAG (fatal)", rc == WMS2_E_BADTAG);
        }

        /* kind=1 ENC flag: authentic outer MAC, no AEAD key -> NO-AEAD */
        {
            const uint8_t *tbody = (const uint8_t *)"ciphertext-bytes";
            size_t tbodylen = strlen((const char *)tbody);
            uint8_t eenv[ENV_SZ];
            int ee = seal_telemetry_local(eenv, sizeof eenv, TTOPIC,
                                          "watcher-01", WMS2_FLAG_ENC, 4, TS,
                                          tbody, tbodylen, key32);
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 120000;
            rc = wms2_verify(eenv, (size_t)ee, TTOPIC, key32, NULL,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("kind1 ENC -> NO-AEAD-KEY", rc == WMS2_E_NOAEAD);
            check("NO-AEAD never advances high-water", rp.high_water == 0);
        }

        /* gate ordering: auth before replay before ts */
        {
            uint8_t old[ENV_SZ], tamperold[ENV_SZ];
            int eo = wms2_seal_control(old, sizeof old, TOPIC, DEV, 2,
                                       TS - 600000, body, bodylen, sk);
            memcpy(tamperold, old, (size_t)eo);
            tamperold[WMS2_HDR_LEN] ^= 0x01;   /* bad sig AND stale ts */
            memset(&rp, 0, sizeof rp);
            rp.high_water = 5;                 /* and rollback */
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 120000;
            rc = wms2_verify(tamperold, (size_t)eo, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("order: auth beats replay+ts -> BAD-SIG", rc == WMS2_E_BADSIG);
            rc = wms2_verify(old, (size_t)eo, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, &rp, &pol,
                             &h, &bp, &bl);
            check("order: replay beats ts -> REPLAY", rc == WMS2_E_REPLAY);
            check("ordering failures keep high-water", rp.high_water == 5);
        }

        /* seal edge cases */
        {
            uint8_t tiny[64];
            n = wms2_seal_control(tiny, sizeof tiny, TOPIC, DEV, 1, TS,
                                  body, bodylen, sk);
            check("seal_control small cap -> negative", n < 0);
            n = wms2_seal_control(env, sizeof env, TOPIC,
                                  "0123456789abcdefg", 1, TS, body, bodylen, sk);
            check("seal_control long dev_id -> negative", n < 0);
        }

        /* scratch too small -> SPACE */
        {
            uint8_t small[8];
            memset(&rp, 0, sizeof rp);
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 120000;
            rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, pk,
                             small, sizeof small, NOW, &rp, &pol, &h, &bp, &bl);
            check("tiny scratch -> SPACE", rc == WMS2_E_SPACE);
        }

        /* rp == NULL: replay gate skipped */
        {
            memset(&pol, 0, sizeof pol);
            pol.strict = true; pol.ts_window_ms = 120000;
            rc = wms2_verify(env, (size_t)elen, TOPIC, NULL, pk,
                             scratch, sizeof scratch, NOW, NULL, &pol,
                             &h, &bp, &bl);
            check("rp NULL -> OK, no replay gate", rc == WMS2_OK);
        }
    }

    /* ---- rc strings + misc ------------------------------------------ */
    {
        struct { wms2_rc rc; const char *s; } m[] = {
            { WMS2_OK, "OK" }, { WMS2_OK_DUP, "OK-DUP" },
            { WMS2_OK_GRACE, "OK-GRACE" }, { WMS2_E_SHORT, "SHORT" },
            { WMS2_E_MAGIC, "MAGIC" }, { WMS2_E_KIND, "KIND" },
            { WMS2_E_BADTAG, "BAD-TAG" }, { WMS2_E_BADSIG, "BAD-SIG" },
            { WMS2_E_REPLAY, "REPLAY" }, { WMS2_E_STALE, "STALE" },
            { WMS2_E_NOKEY, "NO-KEY" }, { WMS2_E_UNSEAL, "UNSEALED" },
            { WMS2_E_SPACE, "SPACE" }, { WMS2_E_NOAEAD, "NO-AEAD-KEY" },
        };
        int ok = 1;
        for (j = 0; j < sizeof m / sizeof m[0]; j++)
            if (strcmp(wms2_rc_str(m[j].rc), m[j].s) != 0) ok = 0;
        check("wms2_rc_str fleet strings", ok);
        check("wms2_auth_bytes_needed",
              wms2_auth_bytes_needed(3, 5) == 2 + 3 + WMS2_HDR_LEN + 5);
        check("E_MAGIC reserved (ref never returns it)",
              wms2_rc_str(WMS2_E_MAGIC) != NULL);
        /* sanity: tag compare is constant-time api (smoke: equal tags) */
        memset(tag, 0x5a, sizeof tag);
        check("crypto_verify16 smoke", crypto_verify16(tag, tag) == 0);
        (void)msgbuf;
    }

    printf("\n%s\n", n_fail ? "FAILURES PRESENT" : "ALL PASS");
    if (n_fail) printf("%d FAILURE(S), %d passed\n", n_fail, n_pass);
    else printf("%d passed\n", n_pass);
    return n_fail ? 1 : 0;
}
