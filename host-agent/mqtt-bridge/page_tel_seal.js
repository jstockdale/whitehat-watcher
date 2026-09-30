/* page_tel_seal.js — WMS2 kind-1 ENC telemetry sealer (D5b, page side).
 *
 * Frame/audio-bearing telemetry lines are sealed into kind-1 ENC
 * envelopes before they enter the MQTT telemetry batch:
 *   {"t":"sealed_chunk","enc":"<base64 WMS2 envelope>"}
 * Pure status lines are untouched (MAC-only eligible, not yet sealed).
 *
 * FAIL-CLOSED (0.4.56): maybeSealRaw never returns a plaintext path.
 * A media line that cannot be sealed (no seed, no crypto bundle, seal
 * error) returns {drop:true}; the queueTelemetry choke point drops the
 * line whole and emits a rate-limited tel_enc_drop notice that carries
 * no media bytes.
 *
 * Requires: window.watcherSealCrypto.blake2b (seal-crypto-bundle.js)
 *           window.watcherSealAead.xchacha20poly1305 (seal-aead-bundle.js)
 *
 * Key derivation (must match mqtt_seal.derive_tel_keys byte-for-byte):
 *   k_tel_mac  = BLAKE2b-256(key=seed, "WMS2-tel-mac-v1")
 *   k_tel_aead = BLAKE2b-256(key=seed, "WMS2-tel-aead-v1")
 *   fp         = hex(BLAKE2b-256(key=seed, "WMS2-tel-fp-v1"))[:16]
 *
 * Envelope (must match mqtt_seal.seal_telemetry_enc byte-for-byte):
 *   header(40): "WMS2" | kind=1 | flags=0x01 | dev_id[16] NUL-padded
 *               | ctr u64 LE | ts_ms u64 LE | rsv u16
 *   body: nonce(24) || XChaCha20-Poly1305(k_aead, nonce, plaintext)
 *   auth(16): BLAKE2b(key=k_mac, len16(topic)||topic||header||body)
 */
var TEL = (function () {
  "use strict";

  var LS_SEED = "watcherTelSeed";
  var LS_CTR = "watcherTelCtr";
  var KIND_TELEM = 1;
  var FLAG_ENC = 0x01;
  var TAG_LEN = 16;
  var NONCE_LEN = 24;
  var DEVID = "page-01";

  var KDF_MAC = "WMS2-tel-mac-v1";
  var KDF_AEAD = "WMS2-tel-aead-v1";
  var KDF_FP = "WMS2-tel-fp-v1";

  function getCrypto() {
    var c = (typeof window !== "undefined") ? window.watcherSealCrypto : null;
    var a = (typeof window !== "undefined") ? window.watcherSealAead : null;
    if (!c || !c.blake2b || !a || !a.xchacha20poly1305) return null;
    return { blake2b: c.blake2b, aead: a.xchacha20poly1305 };
  }

  function hasCrypto() { return !!getCrypto(); }

  function hexToBytes(hex) {
    if (typeof hex !== "string" || !/^[0-9a-fA-F]{64}$/.test(hex)) return null;
    var out = new Uint8Array(32);
    for (var i = 0; i < 32; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
    return out;
  }

  function bytesToHex(b) {
    var s = "";
    for (var i = 0; i < b.length; i++) s += ("0" + b[i].toString(16)).slice(-2);
    return s;
  }

  function b64encode(u8) {
    var s = "";
    for (var i = 0; i < u8.length; i += 8192)
      s += String.fromCharCode.apply(null, u8.subarray(i, i + 8192));
    return btoa(s);
  }

  function b64decode(b64) {
    var s = atob(b64), out = new Uint8Array(s.length);
    for (var i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
    return out;
  }

  function lsGet(k) {
    try { return localStorage.getItem(k); } catch (_e) { return null; }
  }
  function lsSet(k, v) {
    try { localStorage.setItem(k, v); } catch (_e) {}
  }
  function lsDel(k) {
    try { localStorage.removeItem(k); } catch (_e) {}
  }

  /* ---- seed management (the seed itself is never displayed) ---- */

  function getSeed() {
    var hex = lsGet(LS_SEED);
    return hex ? hexToBytes(hex) : null;
  }

  function setSeedHex(hex) {
    var seed = hexToBytes(hex);
    if (!seed) throw new Error("telemetry seed must be 64 hex chars");
    lsSet(LS_SEED, hex.toLowerCase());
    return fingerprint();
  }

  function clearSeed() { lsDel(LS_SEED); }

  function hasSeed() { return !!getSeed(); }

  function kdf(seed, label) {
    var cr = getCrypto();
    if (!cr) throw new Error("seal crypto bundle missing");
    var msg = new TextEncoder().encode(label);
    return cr.blake2b(msg, { dkLen: 32, key: seed });
  }

  function deriveKeys(seed) {
    return { mac: kdf(seed, KDF_MAC), aead: kdf(seed, KDF_AEAD) };
  }

  function fingerprint() {
    var seed = getSeed();
    if (!seed) return null;
    var cr = getCrypto();
    if (!cr) return null;
    var d = cr.blake2b(new TextEncoder().encode(KDF_FP),
                       { dkLen: 32, key: seed });
    return bytesToHex(d).slice(0, 16);
  }

  /* ---- envelope ---- */

  function nextCtr() {
    var c = parseInt(lsGet(LS_CTR) || "0", 10);
    if (!isFinite(c) || c < 0) c = 0;
    c += 1;
    lsSet(LS_CTR, String(c));
    return c;
  }

  function buildHeader(ctr, ts_ms) {
    var hdr = new Uint8Array(40);
    hdr[0] = 0x57; hdr[1] = 0x4D; hdr[2] = 0x53; hdr[3] = 0x32; // "WMS2"
    hdr[4] = KIND_TELEM;
    hdr[5] = FLAG_ENC;
    for (var i = 0; i < DEVID.length && i < 16; i++)
      hdr[6 + i] = DEVID.charCodeAt(i);
    var dv = new DataView(hdr.buffer, hdr.byteOffset, hdr.byteLength);
    dv.setBigUint64(22, BigInt(ctr), true);
    dv.setBigUint64(30, BigInt(ts_ms), true);
    return hdr;
  }

  function authBytes(topic, hdr, body) {
    var t = new TextEncoder().encode(topic);
    var out = new Uint8Array(2 + t.length + hdr.length + body.length);
    out[0] = t.length & 0xff; out[1] = (t.length >> 8) & 0xff;
    out.set(t, 2);
    out.set(hdr, 2 + t.length);
    out.set(body, 2 + t.length + hdr.length);
    return out;
  }

  function randomNonce() {
    var n = new Uint8Array(NONCE_LEN);
    var g = (typeof crypto !== "undefined" && crypto.getRandomValues)
      ? crypto : null;
    if (!g) throw new Error("no secure RNG (crypto.getRandomValues)");
    g.getRandomValues(n);
    return n;
  }

  /* Seal a media object -> raw envelope bytes. opts pins ctr/ts/nonce
   * for deterministic tests. Throws on missing crypto/seed. */
  function sealEnvelope(obj, topic, opts) {
    var cr = getCrypto();
    if (!cr) throw new Error("seal crypto bundle missing");
    var seed = getSeed();
    if (!seed) throw new Error("no telemetry seed provisioned");
    var keys = deriveKeys(seed);
    var pt = new TextEncoder().encode(JSON.stringify(obj));
    var nonce = (opts && opts.nonce) ? opts.nonce : randomNonce();
    var ctr = (opts && opts.ctr != null) ? opts.ctr : nextCtr();
    var ts = (opts && opts.ts != null) ? opts.ts : Date.now();
    var hdr = buildHeader(ctr, ts);
    var ct = cr.aead(keys.aead, nonce).encrypt(pt);
    var body = new Uint8Array(NONCE_LEN + ct.length);
    body.set(nonce, 0);
    body.set(ct, NONCE_LEN);
    var mac = cr.blake2b(authBytes(topic, hdr, body),
                         { dkLen: TAG_LEN, key: keys.mac });
    var env = new Uint8Array(hdr.length + body.length + mac.length);
    env.set(hdr, 0);
    env.set(body, hdr.length);
    env.set(mac, hdr.length + body.length);
    return { envelope: env, ctr: ctr };
  }

  function isMediaObj(o) {
    return !!o && (o.t === "snaptest_chunk" || o.t === "audio");
  }

  /* The queueTelemetry choke point calls this. Fail-closed contract:
   *   {sealed: "<json>"}  media line sealed (never truncate this)
   *   {drop: true, ...}   media line, but no seed / no crypto / seal
   *                       error -> the caller MUST drop the line whole.
   *                       There is no plaintext path.
   *   {skip: true}        not a media line
   */
  function maybeSealRaw(raw, topic) {
    var o;
    try { o = JSON.parse(raw); } catch (_e) { return { skip: true }; }
    if (!isMediaObj(o)) return { skip: true };
    var cr = getCrypto(), seed = getSeed();
    if (!cr || !seed)
      return { drop: true, noCrypto: !cr, noSeed: !seed };
    try {
      var r = sealEnvelope(o, topic);
      return { sealed: JSON.stringify({ t: "sealed_chunk", enc: b64encode(r.envelope) }) };
    } catch (_e) {
      return { drop: true, err: String(_e && _e.message || _e) };
    }
  }

  /* Verify+decrypt a sealed_chunk wrapper (used by tests; the host
   * does this job in production via mqtt_seal.verify). */
  function openEnvelope(b64env, topic, seedOverride) {
    var cr = getCrypto();
    if (!cr) throw new Error("seal crypto bundle missing");
    var seed = seedOverride || getSeed();
    if (!seed) throw new Error("no telemetry seed provisioned");
    var keys = deriveKeys(seed);
    var env = b64decode(b64env);
    if (env.length < 40 + NONCE_LEN + 16 + TAG_LEN)
      throw new Error("SHORT");
    if (env[0] !== 0x57 || env[1] !== 0x4D || env[2] !== 0x53 || env[3] !== 0x32)
      throw new Error("MAGIC");
    if (env[4] !== KIND_TELEM) throw new Error("KIND");
    if (!(env[5] & FLAG_ENC)) throw new Error("not an ENC envelope");
    var hdr = env.subarray(0, 40);
    var body = env.subarray(40, env.length - TAG_LEN);
    var mac = env.subarray(env.length - TAG_LEN);
    var want = cr.blake2b(authBytes(topic, hdr, body),
                          { dkLen: TAG_LEN, key: keys.mac });
    var ok = want.length === mac.length;
    for (var i = 0; i < want.length && i < mac.length; i++)
      ok = ok && (want[i] === mac[i]);
    if (!ok) throw new Error("BAD_TAG");
    var nonce = body.subarray(0, NONCE_LEN);
    var ct = body.subarray(NONCE_LEN);
    var pt;
    try {
      pt = cr.aead(keys.aead, nonce).decrypt(ct);
    } catch (_e) {
      throw new Error("AEAD_FAIL");
    }
    return JSON.parse(new TextDecoder().decode(pt));
  }

  return {
    hasCrypto: hasCrypto,
    getSeed: getSeed,
    setSeedHex: setSeedHex,
    clearSeed: clearSeed,
    hasSeed: hasSeed,
    fingerprint: fingerprint,
    deriveKeys: deriveKeys,
    isMediaObj: isMediaObj,
    sealEnvelope: sealEnvelope,
    maybeSealRaw: maybeSealRaw,
    openEnvelope: openEnvelope,
    DEVID: DEVID,
    _b64encode: b64encode,
    _b64decode: b64decode,
  };
})();
