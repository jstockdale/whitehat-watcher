/* =====================================================================
 * WMS2 envelope verifier for the Watcher console page.
 * Magic "WMS2" is the algorithm registry: kind=2 pins RFC 8032 Ed25519
 * (kind=1 is the keyed-BLAKE2b telemetry MAC; "WMS1" names the
 * incompatible Monocypher EdDSA-BLAKE2b family and never appears here).
 *
 * Verifies sealed command envelopes on watcher/command before they reach
 * the serial dispatch. Uses window.watcherSealCrypto (noble ed25519 +
 * blake2b, vendored bundle inlined above this script).
 *
 * Envelope: HEADER(40) || BODY || AUTH, little-endian.
 *   magic "WMS2", kind u8 (2=control/Ed25519), flags u8, dev_id[16],
 *   ctr u64, ts_ms u64, rsv u16.
 * AUTH covers: len16(topic) || topic || header || body  (topic-bound)
 *
 * Policy (fleet law, adapted):
 *  - bad signature is ALWAYS fatal, every mode
 *  - grace (default): unsigned honored LOUDLY (1-in-64 warn); strict:
 *    unsigned dropped loudly
 *  - ctr equal-to-last = silent dup (QoS echo); ctr <= high_water =
 *    REPLAY, loud; adopt high_water only after ALL gates pass
 *  - |now - ts_ms| <= 120s else STALE (window=0 disables)
 *
 * Replay high-water persists in localStorage per commander id.
 * Strict flag persists in localStorage ("1"/"0").
 *
 * Integration: call sealGuard(payload, topic) at the top of the
 * command-topic branch in mqpOnPublish. It returns:
 *   {ok:true, id, cmd}            verified (or grace-honored) command
 *   {ok:true, dup:true}            QoS echo of last command, ignore
 *   {ok:false, reason}             dropped; reason already logged
 * ===================================================================== */
var SEAL = (function () {
  var LS_PK = "watcherSealCommanderPk";   // hex, pinned by John's paste
  var LS_STRICT = "watcherSealStrict";    // "1" = enforce
  var LS_HW = "watcherSealHighWater";     // JSON {commanderId: ctr}
  var LS_WARN = "watcherSealGraceWarns";  // counter for 1-in-64
  var TS_WINDOW_MS = 120000;
  var COMMANDER_ID = "muse-01";

  function b64ToBytes(b64) {
    var bin = atob(b64), n = bin.length, u = new Uint8Array(n), i;
    for (i = 0; i < n; i++) u[i] = bin.charCodeAt(i);
    return u;
  }
  function hexToBytes(hex) {
    var n = hex.length / 2, u = new Uint8Array(n), i;
    for (i = 0; i < n; i++) u[i] = parseInt(hex.substr(i * 2, 2), 16);
    return u;
  }
  function u64le(u, off) {
    var v = 0, i;
    for (i = 7; i >= 0; i--) v = v * 256 + u[off + i];
    return v;
  }
  function authBytes(topicBytes, hdr, body) {
    var out = new Uint8Array(2 + topicBytes.length + hdr.length + body.length);
    out[0] = topicBytes.length & 255; out[1] = (topicBytes.length >> 8) & 255;
    out.set(topicBytes, 2);
    out.set(hdr, 2 + topicBytes.length);
    out.set(body, 2 + topicBytes.length + hdr.length);
    return out;
  }
  function te(s) { return new TextEncoder().encode(s); }
  function td(u) { return new TextDecoder().decode(u); }

  function getPk() {
    try { return localStorage.getItem(LS_PK) || ""; } catch (e) { return ""; }
  }
  function isStrict() {
    try { return localStorage.getItem(LS_STRICT) === "1"; } catch (e) { return false; }
  }
  function getHighWater() {
    try { return JSON.parse(localStorage.getItem(LS_HW) || "{}"); }
    catch (e) { return {}; }
  }
  function setHighWater(id, ctr) {
    try {
      var hw = getHighWater(); hw[id] = ctr;
      localStorage.setItem(LS_HW, JSON.stringify(hw));
    } catch (e) {}
  }
  function resetHighWater(id) {
    /* Operator control: forget this commander's replay floor
     * (e.g. after a deliberate commander counter reset). */
    try {
      var hw = getHighWater();
      if (id) delete hw[id]; else localStorage.removeItem(LS_HW);
      if (id) localStorage.setItem(LS_HW, JSON.stringify(hw));
    } catch (e) {}
  }

  /* Verify one sealed envelope. Returns {rc, hdr, body, log}.
   * rc: "OK" | "OK_DUP" | "OK_GRACE_NOPIN" | "BAD_SIG" | "REPLAY"
   *     | "STALE" | "NO_KEY" | "NO_CRYPTO" | "SHORT" | "MAGIC" | "KIND"
   * guard() returns {ok:true,id,cmd[,grace]} | {ok:true,dup:true}
   *     | {ok:false,reason} where reason is an rc value, "BAD_INNER",
   *     "UNSEALED", or "malformed".
   *     | "STALE" | "NO_KEY" | "NO_CRYPTO" | "SHORT" | "KIND" | "MAGIC" */
  function verifySealed(b64, topic) {
    var crypto = window.watcherSealCrypto;
    if (!crypto) return { rc: "NO_CRYPTO", log: "SEAL: crypto bundle missing - DROPPED" };
    var env;
    try { env = b64ToBytes(b64); } catch (e) {
      return { rc: "SHORT", log: "SEAL: bad base64 on '" + topic + "' - DROPPED" };
    }
    if (env.length < 104) return { rc: "SHORT", log: "SEAL: SHORT msg on '" + topic + "' - DROPPED" };
    var magic = td(env.slice(0, 4));
    if (magic !== "WMS2") return { rc: "MAGIC", log: "SEAL: bad magic on '" + topic + "' - DROPPED" };
    var kind = env[4];
    if (kind !== 2) return { rc: "KIND", log: "SEAL: bad kind " + kind + " on '" + topic + "' - DROPPED" };
    var devId = td(env.slice(6, 22)).replace(/\0+$/, "");
    var ctr = u64le(env, 22), ts = u64le(env, 30);
    if (ctr > 9007199254740991 || ts > 9007199254740991) {
      /* JS doubles are exact only to 2^53-1; refuse imprecise counters. */
      return { rc: "SHORT", log: "SEAL: counter/timestamp out of JS-safe range on '" + topic + "' - DROPPED" };
    }
    var hdr = env.slice(0, 40);
    var body = env.slice(40, env.length - 64);
    var sig = env.slice(env.length - 64);

    var pkHex = getPk();
    if (!pkHex) {
      if (isStrict()) return { rc: "NO_KEY", hdr: { devId: devId }, log: "SEAL: NO PIN for " + devId + " on '" + topic + "' - DROPPED" };
      return { rc: "OK_GRACE_NOPIN", hdr: { devId: devId }, body: body, log: "SEAL: no pinned key, sealed msg honored (grace)" };
    }
    var msg = authBytes(te(topic), hdr, body);
    var ok = false;
    try {
      ok = crypto.ed25519.verify(sig, msg, hexToBytes(pkHex));
    } catch (e) { ok = false; }
    if (!ok) {
      return { rc: "BAD_SIG", hdr: { devId: devId, ctr: ctr },
               log: "SEAL: BAD SIGNATURE from " + devId + " on '" + topic + "' - DROPPED" };
    }
    /* replay: counter primary; equal-to-last is silent (QoS echo) */
    var hw = getHighWater();
    var last = hw[devId] || 0;
    if (ctr === last && ctr !== 0) return { rc: "OK_DUP", hdr: { devId: devId, ctr: ctr } };
    if (ctr <= last) {
      return { rc: "REPLAY", hdr: { devId: devId, ctr: ctr },
               log: "SEAL: REPLAY ctr " + ctr + " <= " + last + " from " + devId + " - DROPPED" };
    }
    /* freshness: timestamp secondary */
    var dt = Math.abs(Date.now() - ts);
    if (dt > TS_WINDOW_MS) {
      return { rc: "STALE", hdr: { devId: devId, ctr: ctr },
               log: "SEAL: STALE ts (|dt|=" + dt + "ms) from " + devId + " - DROPPED" };
    }
    setHighWater(devId, ctr);  /* adopt only after ALL gates */
    return { rc: "OK", hdr: { devId: devId, ctr: ctr, ts: ts }, body: body };
  }

  /* Grace-mode gate for unsigned payloads. Returns true to honor. */
  function graceHonor(topic) {
    if (isStrict()) return false;
    var n = 0;
    try {
      n = parseInt(localStorage.getItem(LS_WARN) || "0", 10) || 0;
      localStorage.setItem(LS_WARN, String(n + 1));
    } catch (e) {}
    return { honor: true, loud: (n % 64 === 0) };
  }

  /* Main entry: call from mqpOnPublish's command-topic branch.
   * payload = raw MQTT payload string, topic = full topic string.
   * onLog(line) = page's addLine; onSealEvent(obj) = telemetry seal event.
   * Returns {ok:true,id,cmd} | {ok:true,dup:true} | {ok:false,reason} */
  function guard(payload, topic, onLog, onSealEvent) {
    var msg;
    try { msg = JSON.parse(payload); } catch (e) { return { ok: false, reason: "malformed" }; }
    if (msg && typeof msg.sealed === "string") {
      var r = verifySealed(msg.sealed, topic);
      if (r.rc === "OK" || r.rc === "OK_GRACE_NOPIN") {
        if (r.log && onLog) onLog(r.log);
        var inner;
        try { inner = JSON.parse(td(r.body)); }
        catch (e) { if (onLog) onLog("SEAL: verified envelope, bad inner JSON - DROPPED"); return { ok: false, reason: "BAD_INNER" }; }
        if (onSealEvent) onSealEvent({ seal: "verified", from: r.hdr.devId, ctr: r.hdr.ctr });
        return { ok: true, id: inner.id, cmd: inner.cmd, grace: (r.rc === "OK_GRACE_NOPIN") };
      }
      if (r.rc === "OK_DUP") return { ok: true, dup: true };
      if (onLog) onLog("✕ " + (r.log || "SEAL: rejected"));
      if (onSealEvent) onSealEvent({ seal: "rejected", rc: r.rc, log: r.log || "" });
      return { ok: false, reason: r.rc };
    }
    /* unsigned: grace or strict */
    if (isStrict()) {
      var line = "SEAL: UNSEALED msg on '" + topic + "' - DROPPED";
      if (onLog) onLog("✕ " + line);
      if (onSealEvent) onSealEvent({ seal: "rejected", rc: "UNSEALED", log: line });
      return { ok: false, reason: "UNSEALED" };
    }
    var g = graceHonor(topic);
    if (g.loud && onLog) onLog("SEAL: unauthenticated msg on '" + topic + "' honored (grace)");
    if (onSealEvent && g.loud) onSealEvent({ seal: "grace", log: "unsigned honored (grace)" });
    return { ok: true, id: msg && msg.id, cmd: msg && msg.cmd };
  }

  return {
    guard: guard,
    verifySealed: verifySealed,
    getPk: getPk,
    isStrict: isStrict,
    getHighWater: getHighWater,
    resetHighWater: resetHighWater,
    COMMANDER_ID: COMMANDER_ID,
    _lsKeys: { pk: LS_PK, strict: LS_STRICT, hw: LS_HW }
  };
})();
