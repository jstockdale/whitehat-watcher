/* WMS2 kind-2 page verifier test — consumes COMMITTED vectors from
 * vectors/WMS2/kind2/<case>.json (see gen_seal_vectors.py).
 * Time is pinned to the vectors' timestamp so STALE never fires;
 * each case runs with isolated replay state.
 *
 * Cases: valid, tampered-body, cross-topic, wrong-key, bad-magic,
 * duplicate (OK then silent dup), unsigned-grace, unsigned-strict.
 */
"use strict";
const fs = require("fs");

// ---- browser stubs ----
let store = {};
global.window = {};
global.localStorage = {
  getItem: (k) => (k in store ? store[k] : null),
  setItem: (k, v) => { store[k] = String(v); },
  removeItem: (k) => { delete store[k]; },
};

// pin Date.now to the vectors' timestamp (+1s) so freshness is deterministic
const VEC_DIR = __dirname + "/vectors/WMS2/kind2";
const TS_PIN = 1758850000001;
Date.now = () => TS_PIN;

// crypto bundle (noble ed25519+blake2b) and the seal draft under test
(0, eval)(fs.readFileSync(__dirname + "/seal-crypto-bundle.js", "utf8"));
const SEAL = (0, eval)(fs.readFileSync(__dirname + "/page_seal_draft.js", "utf8") + ";SEAL");

const PK = fs.readFileSync(VEC_DIR + "/test-pubkey.hex", "utf8").trim();

let pass = 0, fail = 0;
function check(name, got, want) {
  if (got === want) { pass++; console.log("  ok  " + name); }
  else { fail++; console.log("  FAIL " + name + " got=" + JSON.stringify(got) + " want=" + JSON.stringify(want)); }
}

function freshState() {
  store = {};
  localStorage.setItem("watcherSealCommanderPk", PK);
}

for (const name of ["valid", "tampered-body", "cross-topic", "wrong-key", "bad-magic",
                    "stale", "kind", "short", "ctr-range", "bad-inner"]) {
  freshState();
  const c = JSON.parse(fs.readFileSync(VEC_DIR + "/" + name + ".json", "utf8"));
  const logs = [];
  const r = SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic,
    (l) => logs.push(l), () => {});
  const rc = r.ok ? "OK" : r.reason;
  check(name + "/verdict", rc, c.want);
  if (c.want_log) {
    check(name + "/log", logs.some((l) => l.endsWith(c.want_log)) ? "match" : logs.join("|"), "match");
  }
  if (name === "valid") {
    check(name + "/id+cmd", r.id === "t1" && r.cmd && r.cmd.t === "dbg" ? "extracted" : "bad", "extracted");
  }
}

// replay: adopt ctr=2, then ctr=1 must be a loud REPLAY
{
  freshState();
  const c = JSON.parse(fs.readFileSync(VEC_DIR + "/replay.json", "utf8"));
  const logs = [];
  const r1 = SEAL.guard(JSON.stringify({ sealed: c.b64_first }), c.topic, (l) => logs.push(l), () => {});
  const r2 = SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic, (l) => logs.push(l), () => {});
  check("replay/adopt", r1.ok ? "OK" : "bad", "OK");
  check("replay/verdict", !r2.ok && r2.reason === "REPLAY" ? "REPLAY" : "bad", "REPLAY");
  check("replay/log", logs.some((l) => l.endsWith(c.want_log)) ? "match" : logs.join("|"), "match");
  check("replay/high-water", JSON.stringify(SEAL.getHighWater()), JSON.stringify({ "muse-01": 2 }));
}

// sealed message with NO pinned key: grace honors unsigned-verified, strict drops
{
  store = {};
  const c = JSON.parse(fs.readFileSync(VEC_DIR + "/valid.json", "utf8"));
  const logs = [];
  const r = SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic, (l) => logs.push(l), () => {});
  check("no-key/grace", r.ok && r.grace ? "OK_GRACE_NOPIN" : "bad", "OK_GRACE_NOPIN");
  check("no-key/grace-log", logs.some((l) => l.indexOf("no pinned key") >= 0) ? "warned" : "silent", "warned");
  store = {};
  localStorage.setItem("watcherSealStrict", "1");
  const r2 = SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic, () => {}, () => {});
  check("no-key/strict", !r2.ok && r2.reason === "NO_KEY" ? "NO_KEY" : "bad", "NO_KEY");
}

// missing crypto bundle
{
  freshState();
  const saved = window.watcherSealCrypto;
  delete window.watcherSealCrypto;
  const c = JSON.parse(fs.readFileSync(VEC_DIR + "/valid.json", "utf8"));
  const r = SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic, () => {}, () => {});
  window.watcherSealCrypto = saved;
  check("no-crypto", !r.ok && r.reason === "NO_CRYPTO" ? "NO_CRYPTO" : "bad", "NO_CRYPTO");
}

// malformed outer JSON and bad base64
{
  freshState();
  const r = SEAL.guard("this is not json", "watcher/command", () => {}, () => {});
  check("malformed", !r.ok && r.reason === "malformed" ? "malformed" : "bad", "malformed");
  const logs = [];
  const r2 = SEAL.guard(JSON.stringify({ sealed: "!!!" }), "watcher/command", (l) => logs.push(l), () => {});
  check("bad-b64", !r2.ok && r2.reason === "SHORT" ? "SHORT" : "bad", "SHORT");
}

// duplicate: same committed bytes twice — second call must be a silent dup
{
  freshState();
  const c = JSON.parse(fs.readFileSync(VEC_DIR + "/duplicate.json", "utf8"));
  const logs = [];
  const r1 = SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic, (l) => logs.push(l), () => {});
  const r2 = SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic, (l) => logs.push(l), () => {});
  check("duplicate/first", r1.ok && !r1.dup ? "OK" : "bad", "OK");
  check("duplicate/second", r2.ok && r2.dup ? "OK_DUP" : "bad", "OK_DUP");
  check("duplicate/silent", logs.length === 0 ? "silent" : logs.join("|"), "silent");
}

// unsigned: grace honors, strict drops
{
  freshState();
  const r = SEAL.guard(JSON.stringify({ id: "u1", cmd: { t: "dbg" } }), "watcher/command", () => {}, () => {});
  check("unsigned/grace", r.ok === true ? "honored" : "dropped", "honored");
  localStorage.setItem("watcherSealStrict", "1");
  const r2 = SEAL.guard(JSON.stringify({ id: "u2", cmd: { t: "dbg" } }), "watcher/command", () => {}, () => {});
  check("unsigned/strict", r2.ok === false && r2.reason === "UNSEALED" ? "dropped" : "honored", "dropped");
}

// high-water: exactly one adoption (ctr=1) across the sealed cases run above
{
  freshState();
  const c = JSON.parse(fs.readFileSync(VEC_DIR + "/valid.json", "utf8"));
  SEAL.guard(JSON.stringify({ sealed: c.b64 }), c.topic, () => {}, () => {});
  check("high-water", JSON.stringify(SEAL.getHighWater()), JSON.stringify({ "muse-01": 1 }));
}

console.log(pass + " passed, " + fail + " failed");
process.exit(fail ? 1 : 0);
