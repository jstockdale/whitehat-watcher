#!/usr/bin/env node
/* test_page_failclosed.js — fail-closed E2E for D5b telemetry ENC (0.4.56).
 *
 * Stubs navigator.serial (fake Watcher port) and WebSocket (fake broker),
 * then drives the REAL page with NO telemetry seed provisioned:
 *   1. fp shows NONE; enable telemetry + connect MQTT (fake CONNACK)
 *   2. connect fake serial, inject a snaptest_chunk line
 *   3. capture MQTT PUBLISH packets off the wire
 *   4. assert NO plaintext frame bytes / no snaptest_chunk JSON on wire
 *   5. assert the "Dropped media lines" counter incremented
 *   6. assert the tel_enc_drop warn line exists and carries no media bytes
 *   7. delete the AEAD bundle, inject another chunk, assert still dropped
 * Run: NODE_PATH=/tmp/node_modules node test_page_failclosed.js
 */
"use strict";
const fs = require("fs");
const { JSDOM, VirtualConsole } = require("jsdom");

const PAGE = "/home/hatch/workspace/your_files/watcher-console/watcher-console.html";
const MARKER = "FAKEJPEGDATA-failclosed-marker-plain-bytes";
const TOPIC = "watcher/telemetry";

const html = fs.readFileSync(PAGE, "utf8");
const fails = [];
function check(name, cond, extra) {
  console.log((cond ? "ok  " : "FAIL") + "  " + name);
  if (!cond) fails.push(name + (extra ? " :: " + extra : ""));
}

const sentPackets = [];
let wsInstance = null;
let chunkCount = 0;

function installFakes(window) {
  const mkChunk = () => JSON.stringify({
    t: "snaptest_chunk", id: 7, total: 1, seq: 0, last: true,
    b64: Buffer.from(MARKER).toString("base64"),
  }) + "\n";
  const fakeReader = {
    async read() {
      chunkCount++;
      if (chunkCount <= 4) {
        // pace chunks so phases 1 (no-seed) and 2 (no-crypto) each get two
        await new Promise(r => setTimeout(r, chunkCount === 1 ? 50 : 1200));
        return { value: new TextEncoder().encode(mkChunk()), done: false };
      }
      await new Promise(r => setTimeout(r, 5000));
      return { done: true };
    },
    cancel() {},
  };
  const fakePort = {
    async open() {},
    get readable() { return { getReader: () => fakeReader }; },
    get writable() { return { getWriter: () => ({ write() {}, releaseLock() {} }) }; },
    close() {},
  };
  Object.defineProperty(window.navigator, "serial", {
    value: { requestPort: async () => fakePort }, configurable: true,
  });
  window.WebSocket = function (url, proto) {
    wsInstance = this;
    this.url = url; this.protocol = proto;
    this.binaryType = "arraybuffer";
    this.readyState = 0;
    const self = this;
    setTimeout(() => {
      self.readyState = 1;
      if (self.onopen) self.onopen({});
      setTimeout(() => {
        if (self.onmessage) self.onmessage({ data: new Uint8Array([0x20, 0x02, 0x00, 0x00]).buffer });
      }, 30);
    }, 30);
  };
  window.WebSocket.prototype.send = function (pkt) {
    sentPackets.push(Buffer.from(pkt));
  };
  window.WebSocket.prototype.close = function () { this.readyState = 3; };
  window.confirm = () => true;
}

const vc = new VirtualConsole();
vc.on("jsdomError", e => console.log("JSDOMERROR:", (e.detail && e.detail.message) || e.message));

const dom = new JSDOM(html, {
  url: "http://localhost:8000/watcher-console.html",
  runScripts: "dangerously",
  pretendToBeVisual: true,
  virtualConsole: vc,
  beforeParse: installFakes,
});
const { document } = dom.window;

function mqttDecodePublish(pkt) {
  if (pkt[0] >> 4 !== 3) return null;
  let i = 1, mult = 1, rem = 0, b;
  do { b = pkt[i++]; rem += (b & 127) * mult; mult *= 128; } while (b & 128);
  const tl = (pkt[i] << 8) | pkt[i + 1]; i += 2;
  const topic = Buffer.from(pkt.slice(i, i + tl)).toString();
  i += tl;
  return { topic, payload: Buffer.from(pkt.slice(i)).toString() };
}

setTimeout(() => {
  try {
    const W = dom.window;
    check("no seed provisioned (fp NONE)",
      document.getElementById("telFp").textContent === "NONE");

    W.localStorage.setItem("watcherMqttUrl", "ws://127.0.0.1:1883/mqtt");
    W.localStorage.setItem("watcherMqttTopic", "watcher");
    const telToggle = document.getElementById("telemetryToggle");
    if (telToggle.getAttribute("aria-checked") !== "true") telToggle.click();

    document.getElementById("mqttTest").click();
    setTimeout(() => {
      check("mqtt connected via fake broker", sentPackets.length > 0);
      document.getElementById("connectBtn").click();

      setTimeout(() => {
        try {
          const pubs = sentPackets.map(mqttDecodePublish).filter(Boolean);
          const telePubs = pubs.filter(p => p.topic === TOPIC);
          check("telemetry PUBLISH captured", telePubs.length > 0);
          const wireText = telePubs.map(p => p.payload).join("\n");
          check("no plaintext frame bytes on the wire",
            !wireText.includes(MARKER));
          check("no snaptest_chunk JSON on the wire",
            !wireText.includes("snaptest_chunk"));
          check("no sealed_chunk either (nothing to seal with)",
            !wireText.includes("sealed_chunk"));

          const dropped = document.getElementById("telDropped").textContent;
          check("Dropped media lines counter incremented",
            parseInt(dropped, 10) >= 1, "telDropped=" + dropped);
          check("Sealed chunks counter still zero",
            document.getElementById("telSealed").textContent === "0");

          // the warn line is in the page log; it must carry no media bytes
          const logText = document.getElementById("terminal")
            ? document.getElementById("terminal").textContent : "";
          check("tel_enc_drop warn visible in log",
            logText.includes("tel_enc_drop"));
          check("warn line carries no media bytes",
            !logText.includes(MARKER) &&
            !logText.includes(Buffer.from(MARKER).toString("base64")));

          // now kill the AEAD bundle: sealing must still fail closed.
          // (TEL lives inside the page IIFE, so drive the real Setup UI.)
          const before = document.getElementById("telDropped").textContent;
          delete W.watcherSealAead;
          document.getElementById("telSeed").value =
            [...Array(32).keys()].map(i => ("0" + i.toString(16)).slice(-2)).join("");
          document.getElementById("telProvision").click();
          setTimeout(() => {
            const after = document.getElementById("telDropped").textContent;
            check("crypto-missing chunk also dropped",
              parseInt(after, 10) > parseInt(before, 10),
              "before=" + before + " after=" + after);
            const wire2 = sentPackets.map(mqttDecodePublish).filter(Boolean)
              .filter(p => p.topic === TOPIC).map(p => p.payload).join("\n");
            check("still no plaintext after crypto removal",
              !wire2.includes(MARKER) && !wire2.includes("snaptest_chunk"));
            console.log(fails.length ? "\nFAILURES:\n- " + fails.join("\n- ")
                                     : "\nFAIL-CLOSED E2E: ALL PASS");
            process.exit(fails.length ? 1 : 0);
          }, 2500);
        } catch (e) {
          console.log("FAIL  inspect", e && e.message);
          process.exit(1);
        }
      }, 2500);
    }, 600);
  } catch (e) {
    console.log("FAIL  setup", e && e.message);
    process.exit(1);
  }
}, 1200);
