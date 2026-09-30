#!/usr/bin/env node
/* test_page_tel.js — node tests for page_tel_seal.js (D5b page side).
 * Checks the committed kind1enc vectors, KDF byte-equality, and
 * byte-identical envelopes vs the Python sealer (pinned inputs).
 * Run: node test_page_tel.js
 */
"use strict";
const fs = require("fs");
const path = require("path");
const HERE = __dirname;
const VEC_DIR = path.join(HERE, "vectors", "WMS2", "kind1enc");

// ---- browser shims ----
global.window = {};
const store = {};
global.localStorage = {
  getItem: (k) => (k in store ? store[k] : null),
  setItem: (k, v) => { store[k] = String(v); },
  removeItem: (k) => { delete store[k]; },
};
// node >= 19 already exposes global webcrypto; nothing to shim.

eval(fs.readFileSync(path.join(HERE, "seal-crypto-bundle.js"), "utf8"));
eval(fs.readFileSync(path.join(HERE, "seal-aead-bundle.js"), "utf8"));
const geval = eval; // indirect: run in global scope so `var TEL` lands globally
geval(fs.readFileSync(path.join(HERE, "page_tel_seal.js"), "utf8"));

let fails = [];
function check(name, cond, extra) {
  console.log((cond ? "ok  " : "FAIL") + "  " + name);
  if (!cond) fails.push(name + (extra ? " :: " + extra : ""));
}
function vec(name) {
  return JSON.parse(fs.readFileSync(path.join(VEC_DIR, name + ".json"), "utf8"));
}
function bytesRange(n) {
  const u = new Uint8Array(n);
  for (let i = 0; i < n; i++) u[i] = i;
  return u;
}
function seedHex() { return Buffer.from(bytesRange(32)).toString("hex"); }
function openExpect(name, topic, seedOv, wantErr) {
  const v = vec(name);
  let got = null, obj = null;
  try {
    obj = TEL.openEnvelope(v.b64, topic || v.topic,
                           seedOv ? Uint8Array.from(Buffer.from(seedOv, "hex")) : undefined);
  } catch (e) { got = e.message; }
  if (wantErr) check(name + "/throws-" + wantErr, got === wantErr, "got " + got);
  else check(name + "/opens", got === null, "threw " + got);
  return obj;
}

// crypto bundles present
check("bundles/hasCrypto", TEL.hasCrypto());

// KDF byte-equality with committed vector
const kdf = vec("kdf");
TEL.setSeedHex(kdf.seed_hex);
const keys = TEL.deriveKeys(TEL.getSeed());
check("kdf/mac", Buffer.from(keys.mac).toString("hex") === kdf.want_mac_hex);
check("kdf/aead", Buffer.from(keys.aead).toString("hex") === kdf.want_aead_hex);
check("kdf/fp", TEL.fingerprint() === kdf.want_fp);

// seed validation
let threw = false;
try { TEL.setSeedHex("xyz"); } catch (e) { threw = true; }
check("seed/validation", threw);
TEL.clearSeed();
check("seed/clear", !TEL.hasSeed() && TEL.fingerprint() === null);
TEL.setSeedHex(kdf.seed_hex);

// decrypt committed vectors (plaintext bodies must match)
for (const n of ["valid-enc", "frame-roundtrip", "audio-roundtrip"]) {
  const v = vec(n);
  const obj = openExpect(n);
  check(n + "/body", obj && JSON.stringify(obj) === v.body);
}
// negative vectors
openExpect("wrong-topic", "watcher/command", null, "BAD_TAG");
openExpect("ct-tamper", null, null, "BAD_TAG");
openExpect("outer-tag-tamper", null, null, "BAD_TAG");
openExpect("wrong-seed", null, null, "BAD_TAG"); // sealed under SEED2
// wrong-aead-key: verifier provisions SEED (mac ok), envelope used K_AEAD2
openExpect("wrong-aead-key", null, null, "AEAD_FAIL");
openExpect("short-enc-body", null, null, "SHORT");

// byte-identical envelope: JS seal with pinned inputs == Python vector
const TS = 1758850000000;
const v = vec("valid-enc");
TEL.setSeedHex(v.seed_hex);
const bodyObj = JSON.parse(v.body);
const r = TEL.sealEnvelope(bodyObj, v.topic,
                           { ctr: 1, ts: TS, nonce: bytesRange(24) });
const jsB64 = TEL._b64encode(r.envelope);
check("xlang/envelope-bytes-identical", jsB64 === v.b64,
      "len js=" + jsB64.length + " py=" + v.b64.length);
check("xlang/ctr", r.ctr === 1);

// round trip through our own sealer
const r2 = TEL.sealEnvelope({ t: "audio", seq: 1, b64: "QUJD" },
                            "watcher/telemetry");
const back = TEL.openEnvelope(TEL._b64encode(r2.envelope), "watcher/telemetry");
check("roundtrip/audio", back.t === "audio" && back.seq === 1);

// maybeSealRaw choke-point behavior
const TOPIC = "watcher/telemetry";
const chunkRaw = JSON.stringify({ t: "snaptest_chunk", id: 1, seq: 0,
                                  total: 2, b64: "QUJD", last: false });
let m = TEL.maybeSealRaw(chunkRaw, TOPIC);
check("maybeSeal/chunk-sealed", !!m.sealed);
const wrapped = JSON.parse(m.sealed);
check("maybeSeal/wrapper", wrapped.t === "sealed_chunk" && typeof wrapped.enc === "string");
const opened = TEL.openEnvelope(wrapped.enc, TOPIC);
check("maybeSeal/open", opened.t === "snaptest_chunk" && opened.b64 === "QUJD");
m = TEL.maybeSealRaw(JSON.stringify({ t: "audio", seq: 2, b64: "RA==" }), TOPIC);
check("maybeSeal/audio-sealed", !!m.sealed);
m = TEL.maybeSealRaw(JSON.stringify({ t: "heartbeat", uptime_s: 5 }), TOPIC);
check("maybeSeal/status-skip", m.skip === true);
m = TEL.maybeSealRaw("not json at all", TOPIC);
check("maybeSeal/nonjson-skip", m.skip === true);
TEL.clearSeed();
m = TEL.maybeSealRaw(chunkRaw, TOPIC);
check("maybeSeal/no-seed-drop", m.drop === true && m.noSeed === true && !m.sealed && !m.plain);
m = TEL.maybeSealRaw(JSON.stringify({ t: "audio", seq: 9, b64: "QUJDRA==" }), TOPIC);
check("maybeSeal/no-seed-audio-drop", m.drop === true && m.noSeed === true);
// drop object must carry no media bytes
check("maybeSeal/drop-no-media", JSON.stringify(m).indexOf("QUJD") === -1);
// missing AEAD bundle -> drop, never plaintext
const keepAead = window.watcherSealAead;
window.watcherSealAead = null;
TEL.setSeedHex(v.seed_hex);
m = TEL.maybeSealRaw(chunkRaw, TOPIC);
check("maybeSeal/no-aead-drop", m.drop === true && m.noCrypto === true && !m.sealed);
window.watcherSealAead = keepAead;
// forced seal exception -> drop, never plaintext
const keepXc = window.watcherSealAead.xchacha20poly1305;
window.watcherSealAead.xchacha20poly1305 = function () {
  throw new Error("boom");
};
m = TEL.maybeSealRaw(chunkRaw, TOPIC);
check("maybeSeal/seal-exception-drop", m.drop === true && /boom/.test(m.err || "") && !m.sealed);
window.watcherSealAead.xchacha20poly1305 = keepXc;
// sealed output never embeds the plaintext media bytes
TEL.setSeedHex(v.seed_hex);
m = TEL.maybeSealRaw(chunkRaw, TOPIC);
check("maybeSeal/sealed-no-plaintext-leak", !!m.sealed && m.sealed.indexOf("QUJD") === -1);

console.log(fails.length ? "\nFAILURES:\n- " + fails.join("\n- ")
                         : "\nALL PASS");
process.exit(fails.length ? 1 : 0);
