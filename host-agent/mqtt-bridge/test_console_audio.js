/* test_console_audio.js — synthetic trace harness for the console
 * Privacy-tab audio reassembler, spec B2 of
 * docs/audio-reader-spec-2026-11-09.md.
 *
 * Extracts privacyAudioChunk / privacyAudioEnd / privacyAudioComplete
 * (plus the ADPCM tables, b64ToBytes, adpcmDecode, wavBlob they use)
 * VERBATIM from the shipped console source and runs them under Node
 * with the page globals stubbed (state, updateLastAudio, privacyAdd).
 * The console HTML is read, never modified.
 *
 * Console source: $WATCHER_CONSOLE_HTML or
 * ~/workspace/your_files/watcher-console/watcher-console.html
 * (the shipped copy named by docs/RUNBOOK.md:497).
 *
 * Traces mirror test_audio_handler.py (B1): (i) legacy constant
 * total, (ii) streaming total-only-on-final, (iii) final chunk lost +
 * audio_end adoption, (iv) final chunk AND audio_end lost — the R3
 * silent-stranding case: the console has no sweep, and this harness
 * asserts the stranding so it stays documented until B5, (v) total: 0
 * — console already guards (j.total > 0), expected immune, (vi)
 * duplicate chunks, (vii) mid-stream hole, (viii) interleaved takes,
 * (ix) out-of-order delivery.
 *
 * Run: node test_console_audio.js
 */
"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");

const HTML_PATH = process.env.WATCHER_CONSOLE_HTML ||
  path.join(os.homedir(), "workspace/your_files/watcher-console/watcher-console.html");
const html = fs.readFileSync(HTML_PATH, "utf8");

// ---- verbatim extraction from the shipped source ----
function extractFn(src, name, beforeIdx) {
  const needle = "function " + name + "(";
  let idx = src.indexOf(needle);
  if (beforeIdx != null) { // last definition that starts before beforeIdx
    let last = -1;
    while (idx !== -1 && idx < beforeIdx) { last = idx; idx = src.indexOf(needle, idx + 1); }
    idx = last;
  }
  if (idx === -1) throw new Error("not found: " + name);
  let i = src.indexOf("{", idx), depth = 0;
  for (let j = i; j < src.length; j++) {
    if (src[j] === "{") depth++;
    else if (src[j] === "}") { depth--; if (!depth) return src.slice(idx, j + 1); }
  }
  throw new Error("unbalanced: " + name);
}
function extractVar(src, name) {
  const m = src.match(new RegExp("var " + name + "=\\[[^\\]]*\\];"));
  if (!m) throw new Error("not found: var " + name);
  return m[0];
}

const privacyIdx = html.indexOf("function privacyAudioChunk(");
const shimSrc = [
  extractVar(html, "ADPCM_STEP"),
  extractVar(html, "ADPCM_IDX"),
  extractFn(html, "b64ToBytes", privacyIdx),
  extractFn(html, "adpcmDecode"),
  extractFn(html, "wavBlob"),
  extractFn(html, "privacyAudioChunk"),
  extractFn(html, "privacyAudioEnd"),
  extractFn(html, "privacyAudioComplete"),
].join("\n");

// page-global stubs: completions are observed via updateLastAudio /
// privacyAdd, exactly the two exits privacyAudioComplete uses.
const shim = new Function(
  "state", "updateLastAudio", "privacyAdd",
  shimSrc +
  "\nreturn { privacyAudioChunk, privacyAudioEnd, privacyAudioComplete };");

// ---- harness scaffolding (sibling-test style) ----
let pass = 0, fail = 0;
const failures = [];
function check(name, cond, extra) {
  if (cond) { pass++; console.log("ok    " + name); }
  else {
    fail++; failures.push(name);
    console.log("FAIL  " + name + (extra ? " :: " + extra : ""));
  }
}

// deterministic payload set, same shape as the B1 harness
const SIZES = [1536, 1536, 1536, 1536, 700];
const N = SIZES.length, MS = 5000;
function payload(seq, salt) {
  const u = new Uint8Array(SIZES[seq]);
  for (let k = 0; k < u.length; k++)
    u[k] = (seq * 101 + salt * 57 + k * 31 + ((k >> 3) * 7)) & 0xff;
  return u;
}
const PAYLOADS = SIZES.map((_, s) => payload(s, 1));
const PAYLOADS_B = SIZES.map((_, s) => payload(s, 2));
const b64 = (u) => Buffer.from(u).toString("base64");

function freshReader() {
  const state = { audioChunks: {} };
  const done = [], adds = [];
  const api = shim(state,
    (blob, dur, sr, ch) => done.push({ blob, dur, sr, ch }),
    (kind, blob, ext, note) => adds.push({ kind, ext, note }));
  return { state, done, adds, ...api };
}
function chunk(r, id, seq, total, payloads) {
  const j = { t: "audio", id, seq, ms: MS,
              data: b64((payloads || PAYLOADS)[seq]) };
  if (total !== undefined) j.total = total;
  r.privacyAudioChunk(j);
}
function end(r, id, chunks) {
  r.privacyAudioEnd({ t: "audio_end", id, chunks: chunks == null ? N : chunks, ms: MS });
}
async function wavBytes(doneEntry) {
  return Buffer.from(await doneEntry.blob.arrayBuffer());
}

(async () => {
  // (i) legacy: constant total on every chunk
  let r = freshReader();
  for (let s = 0; s < N; s++) chunk(r, "i-legacy", s, N);
  check("i/completes-once-on-final-chunk", r.done.length === 1,
        "done=" + r.done.length);
  end(r, "i-legacy");
  check("i/audio_end-after-complete-is-noop", r.done.length === 1);
  const FULL_WAV = r.done.length ? await wavBytes(r.done[0]) : null;
  check("i/note-reports-5/5",
        r.adds.length === 1 && r.adds[0].note.includes("5/5 chunks"),
        JSON.stringify(r.adds));

  // (ii) streaming: total only on the final chunk
  r = freshReader();
  for (let s = 0; s < N - 1; s++) chunk(r, "ii-stream", s);
  check("ii/no-completion-without-total", r.done.length === 0);
  chunk(r, "ii-stream", N - 1, N);
  check("ii/completes-on-final-chunk", r.done.length === 1);
  end(r, "ii-stream");
  check("ii/audio_end-after-complete-is-noop", r.done.length === 1);
  check("ii/wav-identical-to-legacy",
        r.done.length === 1 && FULL_WAV &&
        (await wavBytes(r.done[0])).equals(FULL_WAV));

  // (iii) final chunk lost: audio_end adopts its count
  r = freshReader();
  for (let s = 0; s < N - 1; s++) chunk(r, "iii-finlost", s);
  end(r, "iii-finlost", N);
  check("iii/completes-on-audio_end", r.done.length === 1,
        "done=" + r.done.length);
  check("iii/note-reports-4/5",
        r.adds.length === 1 && r.adds[0].note.includes("4/5 chunks"),
        JSON.stringify(r.adds));

  // (iv) final chunk AND audio_end lost: the R3 stranding.
  // The console reassembler has NO sweep (spec R3/B5): the slot must
  // still be sitting in state.audioChunks, uncompleted, forever.
  // This asserts the observed behaviour so the gap stays documented.
  r = freshReader();
  for (let s = 0; s < N - 1; s++) chunk(r, "iv-strand", s);
  check("iv/no-completion-without-end", r.done.length === 0);
  check("iv/slot-stranded-in-state",
        !!r.state.audioChunks["iv-strand"] &&
        r.state.audioChunks["iv-strand"].got === N - 1,
        JSON.stringify(Object.keys(r.state.audioChunks)));
  check("iv/nothing-in-privacy-tab", r.adds.length === 0);

  // (v) total: 0 — console guards with j.total > 0, so 0 reads as
  // absent: no early completion, take completes via audio_end whole.
  r = freshReader();
  chunk(r, "v-zero", 0, 0);
  check("v/no-completion-on-total-0", r.done.length === 0,
        "done=" + r.done.length);
  for (let s = 1; s < N; s++) chunk(r, "v-zero", s);
  end(r, "v-zero", N);
  check("v/completes-whole-on-audio_end", r.done.length === 1 &&
        r.adds.length === 1 && r.adds[0].note.includes("5/5 chunks"),
        JSON.stringify(r.adds));
  check("v/wav-identical-to-legacy",
        r.done.length === 1 && FULL_WAV &&
        (await wavBytes(r.done[0])).equals(FULL_WAV));

  // (vi) duplicates while the slot is live
  r = freshReader();
  for (let s = 0; s < N - 1; s++) { chunk(r, "vi-dup", s); chunk(r, "vi-dup", s); }
  chunk(r, "vi-dup", N - 1, N);
  end(r, "vi-dup");
  check("vi/exactly-one-completion", r.done.length === 1,
        "done=" + r.done.length);
  check("vi/wav-identical-to-legacy",
        r.done.length === 1 && FULL_WAV &&
        (await wavBytes(r.done[0])).equals(FULL_WAV));

  // (vii) mid-stream hole: completes on audio_end, got/total tells
  r = freshReader();
  for (const s of [0, 1, 3]) chunk(r, "vii-hole", s);
  chunk(r, "vii-hole", 4, N);
  check("vii/no-completion-with-hole", r.done.length === 0);
  end(r, "vii-hole", N);
  check("vii/completes-on-audio_end", r.done.length === 1);
  check("vii/note-reports-4/5",
        r.adds.length === 1 && r.adds[0].note.includes("4/5 chunks"),
        JSON.stringify(r.adds));

  // (viii) interleaved takes, distinct payloads
  r = freshReader();
  for (let s = 0; s < N; s++) {
    chunk(r, "viii-A", s, s === N - 1 ? N : undefined);
    chunk(r, "viii-B", s, s === N - 1 ? N : undefined, PAYLOADS_B);
  }
  check("viii/both-complete-exactly-once", r.done.length === 2,
        "done=" + r.done.length);
  const wavs = [];
  for (const d of r.done) wavs.push(await wavBytes(d));
  check("viii/no-cross-talk",
        r.done.length === 2 && FULL_WAV &&
        wavs.some((w) => w.equals(FULL_WAV)) &&
        !wavs[0].equals(wavs[1]));

  // (ix) out-of-order, total-carrying chunk arrives early
  r = freshReader();
  for (const s of [2, 0, 4, 1]) chunk(r, "ix-ooo", s, s === 4 ? N : undefined);
  check("ix/no-completion-before-last-needed", r.done.length === 0);
  chunk(r, "ix-ooo", 3);
  check("ix/completes-when-set-is-full", r.done.length === 1);
  check("ix/wav-identical-to-legacy",
        r.done.length === 1 && FULL_WAV &&
        (await wavBytes(r.done[0])).equals(FULL_WAV));

  console.log("\n" + pass + " passed, " + fail + " failed" +
              (fail ? " :: " + failures.join(", ") : ""));
  process.exit(fail ? 1 : 0);
})().catch((e) => { console.error("HARNESS ERROR", e); process.exit(2); });
