// test_page_layout.js — regression test for the Setup page / topbar layout.
//
// Guards the 2026-09-25 layout breakage: the "Telemetry encryption"
// .setup-row was nested inside the command-auth row's 250px grid column
// (and the outer row was never closed), which swallowed every following
// row as auto-placed grid items and squeezed them into a thin rail.
// Also guards the topbar: the seal status lives as small text under the
// Connect button (.seal-sub), never as a wide pill in the topbar grid.
//
// Requires: jsdom (npm install jsdom). Skips cleanly without it.
"use strict";
const fs = require("fs");
const path = require("path");

let JSDOM;
try {
  JSDOM = require("jsdom").JSDOM;
} catch (e) {
  console.log("SKIP: jsdom not installed (npm install jsdom)");
  process.exit(0);
}

const PAGE = path.resolve(__dirname, "../../../../your_files/watcher-console/watcher-console.html");
const html = fs.readFileSync(PAGE, "utf8");
const d = new JSDOM(html).window.document;

let pass = 0, fail = 0;
const ok = (name, cond) => {
  if (cond) pass++; else { fail++; console.log("  FAIL " + name); }
  if (cond) console.log("  ok   " + name);
};

// No .setup-row may be nested inside another .setup-row.
let nested = 0;
d.querySelectorAll(".setup-row").forEach(r => {
  if (r.parentElement && r.parentElement.closest(".setup-row")) nested++;
});
ok("no nested .setup-row", nested === 0);

// Every .setup-row must be a direct child of the setup section —
// an unclosed row would swallow its siblings as grid items.
const setupSec = d.getElementById("setup");
const allRows = d.querySelectorAll("#setup .setup-row");
const directRows = [...setupSec.children].filter(e => e.classList && e.classList.contains("setup-row"));
ok("all setup-rows are direct section children", directRows.length === allRows.length);

// The three seal sections are full-width sibling rows, in order.
const sealRows = [...allRows].filter(r => r.classList.contains("seal-block"));
ok("three seal-block rows", sealRows.length === 3);
ok("command-auth seal row first",
  sealRows[0] && sealRows[0].querySelector("h2").textContent.includes("Command authentication"));
ok("telemetry seal row second",
  sealRows[1] && sealRows[1].querySelector("h2").textContent.includes("Telemetry encryption"));
ok("device provisioning row third",
  sealRows[2] && sealRows[2].querySelector("h2").textContent.includes("Device provisioning"));

// Seal status sits under the MQTT status pill in the pill's own grid column;
// the baud indicator has its own line under the Connect button.
ok("sealPill removed from topbar", !d.getElementById("sealPill"));
const sub = d.getElementById("sealSub");
ok("sealSub exists", !!sub);
const museStack = d.querySelector(".topbar .muse-stack");
ok("muse-stack wraps pill + seal status", !!museStack);
ok("sealSub lives in muse-stack (pill's grid column)",
  !!museStack && !!sub && sub.parentElement === museStack);
const museOrder = [...museStack.children].map(e => e.id || e.className);
ok("muse-stack order: pill, seal-sub", JSON.stringify(museOrder) === JSON.stringify(["musePill", "sealSub"]));
ok("baudText still a button (click to change)", d.getElementById("baudText").tagName === "BUTTON");
const stackOrder = [...d.querySelector(".conn-stack").children].map(e => e.id || e.className);
ok("conn-stack children: button + baud", JSON.stringify(stackOrder) === JSON.stringify(["connectBtn", "baudText"]));
const cssText = d.querySelector("style").textContent;
const sealPx = /\.seal-sub\{[^}]*font:700 (\d+)px/.exec(cssText);
const baudPx = /\.baud-text\{[^}]*font:700 (\d+)px/.exec(cssText);
ok("seal-sub and baud-text share one size", !!sealPx && !!baudPx && sealPx[1] === baudPx[1]);
ok("seal-sub and baud-text nudge down onto the CONSOLE baseline",
  /\.seal-sub\{[^}]*margin-top:9px/.test(cssText) && /\.baud-text\{[^}]*margin-top:9px/.test(cssText));
ok("device provisioning section exists with five rows (incl. device name)",
  (html.match(/class="prov-row"/g)||[]).length===5);
ok("every prov row puts its status pill on its own line under the label",
  (html.match(/class="prov-pill-line"/g)||[]).length===5);
ok("sync buttons sit in the title row (prov-head), right of the title",
  (html.match(/class="prov-head"/g)||[]).length===5 &&
  (html.match(/class="secondary prov-sync"/g)||[]).length===5);
["provKeySync","provTelSync","provWifiSync","provMqttSync","provNameSync",
 "provKeyState","provTelState","provWifiState","provMqttState","provNameState",
 "provKeyFp","provTelFp","provWifiSsid","provWifiPass",
 "provMqttUrl","provMqttPrefix","provMqttUser","provMqttPass","provName"
].forEach(function(id){ ok("provisioning element #"+id+" exists", !!d.getElementById(id)); });
ok("stale provisioning placeholders removed",
  !/Wi-Fi \+ device name/.test(html) && !/Provisioning fields/.test(html) && !/class="coming"/.test(html));
ok("device MQTT row has four fields (broker url, topic prefix, username, password)",
  ["provMqttUrl","provMqttPrefix","provMqttUser","provMqttPass"].every(function(id){return !!d.getElementById(id)}));
ok("sync-pill CSS states defined",
  /\.sync-pill\.ok/.test(cssText) && /\.sync-pill\.warn/.test(cssText) &&
  /\.sync-pill\.bad/.test(cssText) && /\.sync-pill\.busy/.test(cssText));
const jsText = [...d.querySelectorAll("script")].map(s => s.textContent).join("\n;\n");
ok("provisioning ack hook present", /provAck\(j\)/.test(jsText) && /provPending\[j\.cmd\]/.test(jsText));
ok("provisioning send path redacts secrets in the log",
  /value not logged/.test(jsText) && !/addLine\("→ "\+text\)/.test(jsText.split("async function provSend")[1].split("function provWait")[0]));
const conn = d.querySelector(".topbar > .conn");
ok("topbar left cell is .conn", !!conn);
ok("conn order: lamp, title, note, version", !!conn &&
  JSON.stringify([...conn.children].map(e => e.id || e.className)) ===
  JSON.stringify(["lamp","connectionTitle","connectionNote","conn-ver"]));
ok("connVer value element present", !!d.getElementById("connVer"));
ok("lamp shares the title row (grid-row:1, col 1)",
  /\.conn \.lamp\{[^}]*grid-column:1;grid-row:1/.test(cssText));
ok("title/note/version stack in the text column (grid-column:2)",
  /#connectionTitle\{[^}]*grid-column:2;grid-row:1/.test(cssText) &&
  /#connectionNote\{[^}]*grid-column:2;grid-row:2/.test(cssText) &&
  /\.conn-ver\{[^}]*grid-column:2;grid-row:3/.test(cssText));
ok("title text style is the larger 750 13px (not a pill)",
  /#connectionTitle\{[^}]*font:750 13px/.test(cssText) && !/\.conn-pill/.test(cssText));
ok("title sits 8px down (0.4.70's 11px read low)",
  /\.conn\{[^}]*padding-top:8px/.test(cssText));
ok("note carries the margin that lands conn-ver on the SEAL/BAUD baseline",
  /#connectionNote\{[^}]*margin-bottom:3px/.test(cssText));
ok("both stacks top-align in topbar",
  /\.fw-badge,\.ka-pill,\.muse-stack,\.conn-stack\{align-self:start\}/.test(cssText));
ok("sub-lines centered under their pill/button",
  /\.muse-stack,\.conn-stack\{display:grid;gap:8px;justify-items:center/.test(cssText));
// Jiggler row: five controls need more than the standard 250px column.
const jgRow = d.querySelector(".setup-row.jg-row");
ok("jiggler row carries jg-row class", !!jgRow);
ok("jiggler controls all present",
  !!d.getElementById("jgInterval") && !!d.getElementById("jgPx") &&
  !!d.getElementById("jgSubpixel") && !!d.getElementById("jgObserve"));
ok("jg-row gets a wider control column on desktop",
  /@media\(min-width:761px\)\{\.setup-row\.jg-row\{grid-template-columns:minmax\(0,1fr\) 440px\}\}/.test(cssText));
ok("jg-row fields wrap with the status on its own line",
  /\.setup-row\.jg-row \.inline-fields\{flex-wrap:wrap/.test(cssText) &&
  /\.setup-row\.jg-row \.jiggle-observe\{flex-basis:100%/.test(cssText));
const topKids = [...d.querySelector(".topbar").children]
  .filter(e => !(e.tagName === "DIV" && e.id && (e.id.indexOf("baudPopover") === 0 || e.id.indexOf("sealPopover") === 0)));
ok("topbar has exactly 6 grid children", topKids.length === 6);

// All interactive seal/tel element ids the script wires up still exist.
["sealPk","sealPin","sealFp","sealStrict","sealHw","sealVerified","sealRejected",
 "sealBadSig","sealDevState","sealLog","telSeed","telProvision","telClear","telFp",
 "telSealed","telDropped","sealSub","sealSubText"].forEach(id =>
  ok("element #" + id + " present", !!d.getElementById(id)));

// No literal JS-style \uXXXX escapes in static markup: HTML renders them
// as-is (0.4.63: the seal card showed "\u2014"/"\u00b7" as text). Escapes
// inside <script> are fine — JS interprets them.
const markupOnly = html.replace(/<script[\s\S]*?<\/script>/gi, "");
const strayEscapes = markupOnly.match(/\\u[0-9a-fA-F]{4}/g) || [];
ok("no literal \\uXXXX escapes in static markup", strayEscapes.length === 0);

/* --- 0.4.78: pills show full text, no truncation; heights restored --- */
ok("sync pills fixed width (20ch) so all five line up in a column",
  /\.sync-pill\{[^}]*width:20ch/.test(cssText));
const musePillRule = (cssText.match(/\.muse-pill\{[^}]*\}/) || [""])[0];
ok("muse status pill min-width 25ch covers longest built-in state (MQTT RECONNECTING)",
  /min-width:25ch/.test(musePillRule));
ok("muse status pill is not fixed-width (may grow so a long custom verb shows in full)",
  !/(^|[;{])width:/.test(musePillRule));
ok("muse status pill text never truncates (no ellipsis rule on .muse-pill b)",
  !/\.muse-pill b\{[^}]*text-overflow:ellipsis/.test(cssText));
ok("topbar toggle pills pin their ON/OFF/AUTO text width",
  /\.ka-pill b\{[^}]*min-width:5ch/.test(cssText));
ok("status-pill values may wrap again (no nowrap/ellipsis on .status-pill b)",
  !/\.status-pill b\{[^}]*(white-space:nowrap|text-overflow:ellipsis)/.test(cssText));

/* --- 0.4.79: every pill renders at the MUSE pill's height --- */
ok("muse pill, connect button, ka-pills and FW badge share height:42px (zoomed together)",
  /\.muse-stack \.muse-pill,.conn-stack #connectBtn,.topbar \.ka-pill,.topbar \.fw-badge\{height:42px;box-sizing:border-box\}/.test(cssText));
ok("provisioning sync-pills render at the same height (35px = 42px x topbar zoom)",
  /\.sync-pill\{[^}]*height:35px/.test(cssText));

/* --- 0.4.80: topbar FW badge matches the pill height --- */
ok("topbar FW badge shares the 42px pill height",
  /,\.topbar \.fw-badge\{height:42px;box-sizing:border-box\}/.test(cssText));
ok("topbar FW badge vertically centers its text",
  /\.topbar \.fw-badge\{display:inline-flex;align-items:center\}/.test(cssText));

/* --- 0.4.81: bridge-down pill text --- */
ok("bridge-down pill reads MUSE BRIDGE DOWN",
  /textContent="MUSE BRIDGE DOWN"/.test(html));

/* --- 0.4.82: device MQTT fields follow the broker section --- */
ok("device-MQTT follow-the-broker sync function exists",
  /function provMqttSyncFromPage\(\)/.test(html));
ok("first sync runs after the broker fields are restored from storage",
  /\$\("mqttTopic"\)\.value=_mc\.topic;provMqttSyncFromPage\(\);/.test(html));
ok("copy-from-broker-section button exists",
  /id="provMqttCopy"/.test(html));
ok("old one-shot prefill is gone",
  !/provMqttPrefill/.test(html));

/* --- 0.4.84: page-side fw-transfer commands are exempt from devVerify forwarding --- */
ok("seal peek helper exists (no-verify routing for sealed envelopes)",
  /function _sealPeekT\(b64\)/.test(html));
ok("peek checks WMS2 magic + kind 2 and reads the plaintext body",
  /0x57\|\|env\[1\]!==0x4D/.test(html)&&/env\[4\]!==2/.test(html)&&/env\.slice\(40,env\.length-64\)/.test(html));
ok("devVerify forwarding exempts all page-side commands",
  /_pt==="fwbegin"\|\|_pt==="fwchunk"\|\|_pt==="fwend"\|\|\s*\n?\s*_pt==="flash"\|\|_pt==="report_fiducials"\|\|_pt==="jiggle"/.test(html));
ok("non-page sealed commands still forward via sealed_fwd in devVerify mode",
  /sendObject\(\{t:"sealed_fwd",topic:topic,b64:_pe\.sealed\}\)/.test(html));
ok("console version is 0.4.96",
  /CONSOLE_VERSION="0.4.96"/.test(html));
/* --- 0.4.90: flashUI drives the topbar during flashing --- */
ok("flashUI sets topbar to Flashing during active stages",
  /ct\.textContent="Flashing"/.test(html));
ok("flashUI shows Console disconnected / Flasher connected note",
  /cn\.textContent="Console disconnected/.test(html));
ok("flashUI sets Flash complete title on success",
  /ct\.textContent="Flash complete"/.test(html));
ok("flashUI sets Flash failed title on error",
  /ct\.textContent="Flash failed"/.test(html));
/* --- 0.4.89: flash progress throttle has no time gate --- */
ok("flash progress fires on 10pct boundaries without a time gate",
  /if\(p>=100\|\|p-_lp>=10\|\|p<_lp\)\{_lp=p;flashTel\("progress"/.test(html));
ok("flash progress throttle has no 3s gate",
  !/_lt=0;return function\(_i,w,total\)/.test(html));
/* --- 0.4.88: flash log forwarded over MQTT telemetry --- */
ok("flashTel helper defined and uses queueTelemetry",
  /function flashTel\(stage,msg,level\)\{if\(!state\.telemetry\)return;try\{queueTelemetry\(\{raw:"flash "\+stage/.test(html));
ok("flash() publishes bootloader connect telemetry",
  /flashTel\("bootloader","connected: "\+chip\)/.test(html));
ok("flash() publishes flash start with sha",
  /flashTel\("start",fwName\+" "\+fwBytes\.length\+"B sha256 "/.test(html));
ok("flash() throttles progress telemetry",
  /flashTel\("progress",p\+"% \("/.test(html));
ok("flash() publishes verify/complete/error telemetry",
  /flashTel\("verify","MD5 match"\)/.test(html)&&/flashTel\("complete",fwName\+" written\+verified"\)/.test(html)&&/flashTel\("error",\(e&&e\.message\?e\.message:String\(e\)\),"error"\)/.test(html));
/* --- 0.4.87: seal_status on connect + updateReady feeds the seal pill --- */
ok("connect requests seal_status after version",
  /sendObject\(\{t:"dbg",cmd:"version"\}\);await sendObject\(\{t:"dbg",cmd:"seal_status"\}\)/.test(html));
ok("updateReady feeds updateDeviceSeal",
  /function updateReady\(j\)\{[^}]*updateDeviceSeal\(j\)/.test(html));
/* --- 0.4.85: MQTT-sync rejection re-renders the pill; pageshot waits for video metadata --- */
/* --- 0.4.86: jiggle split — bridge publishes to watcher/jiggle_bridge; page listens there --- */
ok("jgBridgeTopic helper defined",
  /function jgBridgeTopic\(\)\{return mqttCfg\(\)\.topic\+"\/jiggle_bridge"\}/.test(html));
ok("page subscribes to jgBridgeTopic on CONNACK",
  /mqpSubPkt\(jgBridgeTopic\(\)\)/.test(html));
ok("remote jiggler dispatch listens on jgBridgeTopic",
  /if\(topic===jgBridgeTopic\(\)\)\{jgOnRemoteMsg\(payload\);return\}/.test(html));
ok("page pill state still publishes to jgTopic (page-owned)",
  /function jgPublish\(by\)\{[^}]*mqttPublish\(jgTopic\(\)/.test(html));
ok("mqtt sequence fail callback re-renders (no stuck SYNCING pill)",
  /function\(j\)\{toast\("Device rejected "\+st\[0\]\+": "\+provErrMsg\(st\[0\],j\),true\);provRender\(\);res\(\)\}/.test(html));
ok("pageshot waits for loadedmetadata before reading video dimensions",
  /v\.onloadedmetadata=function/.test(html)&&/v\.videoWidth>2&&v\.videoHeight>2/.test(html));
ok("pageshot refuses a degenerate frame instead of offering a blank PNG",
  /if\(!shotOk\(bmp\)\)\{say\("Capture produced no image/.test(html));
/* --- 0.4.83: device MQTT password follows the broker section too --- */
ok("password has its own dirty flag",
  /var provMqttDirty=\{url:false,prefix:false,user:false,pass:false\}/.test(html));
ok("broker password copies into the device password field (never trimmed)",
  /\$\("mqttPass"\)/.test(html)&&/provMqttDirty\.pass/.test(html)&&/never trimmed: password whitespace is significant/.test(html));
ok("hand-typed device password takes it off auto-sync",
  /\["provMqttPass","pass"\]/.test(html));
ok("broker password edits re-sync the device field",
  /\["mqttUrl","mqttTopic","mqttUser","mqttPass"\]/.test(html));
ok("copy-from-broker-section resets the password dirty flag",
  /provMqttDirty=\{url:false,prefix:false,user:false,pass:false\};\s*\n\s*provMqttSyncFromPage\(\);/.test(html));
/* --- 0.4.82: prov ack correlation survives the firmware 24-byte _mid buffer --- */
ok("prov _mid is short enough to survive firmware echo truncation",
  /var\ mid="pm"\+\(\+\+provMidSeq\)\.toString\(36\)\+Date\.now\(\)\.toString\(36\)\.slice\(\-4\)/.test(html));
ok("unknown-command rejections explain the firmware may predate the command",
  /function provErrMsg/.test(html)&&/does not implement/.test(html));
ok("prov sync timeout is under the firmware 15s sync deadline",
  /var PROV_TIMEOUT_MS=10000;/.test(html)&&/under the firmware's 15s sync deadline/.test(html));
ok("prov registers its ack BEFORE the serial write (no lost-ack race)",
  /provPending\[cmd\]=\{mid:mid,onOk:null,onErr:null,early:null,timer:0\};/.test(html));
ok("early device replies are stashed and delivered, not dropped",
  /if\(!p\.onOk\)\{p\.early=j;return\}/.test(html)&&/if\(p\.early\)\{var\ _j=p\.early;p\.early=null;provDeliver\(cmd,_j\)\}/.test(html));



/* --- 0.4.91: About dialog + changelog --- */
ok("About button exists in Setup",
  /id="aboutBtn"/.test(html));
ok("About modal exists with credits",
  /id="aboutPrompt"/.test(html)&&/John Stockdale and Off by One, Inc/.test(html));
ok("About credits Muse as development partner",
  /Muse \(Meta's AI\) as development partner/.test(html));
ok("About notes possible future open-source",
  /may be open-sourced in the future/.test(html));
ok("CHANGELOG data present with 0.4.91 entry",
  /var CHANGELOG=\[/.test(html)&&/\{v:"0.4.91"/.test(html));
ok("About button opens the dialog",
  /\$\("aboutBtn"\)\.addEventListener\("click",function\(\)\{renderAbout\(\);\$\("aboutPrompt"\)\.hidden=false\}\)/.test(html));
ok("About close button hides the dialog",
  /\$\("aboutPromptClose"\)\.addEventListener\("click",function\(\)\{\$\("aboutPrompt"\)\.hidden=true\}\)/.test(html));

/* --- 0.4.92: About modal width + heading contrast --- */
ok("About modal uses modal-wide class",
  /id="aboutPrompt"[^>]*><div class="modal modal-wide"/.test(html));
ok("modal-wide max-width is 720px",
  /\.modal-wide\{max-width:720px\}/.test(html));
ok("Modal h2 headings are electric green",
  /\.modal h2\{[^}]*color:#4ade80/.test(html));
ok("Modal h3 headings are electric green",
  /\.modal h3\{[^}]*color:#4ade80/.test(html));
ok("Changelog version rows are electric green",
  /\.about-changelog \.chg-v\{[^}]*color:#4ade80/.test(html));
ok("Changelog scroll area is taller (340px)",
  /\.about-changelog\{max-height:340px/.test(html));
ok("CHANGELOG has 0.4.92 entry",
  /\{v:"0.4.92"/.test(html));

/* --- 0.4.93: disconnected provisioning pills --- */
ok("provRender overrides all pills to DISCONNECTED when not connected",
  /if\(!connected\)\{[^}]*provPill\(id,"muted","DISCONNECTED"/.test(html));
ok("disconnected override covers all five provisioning pills",
  /"provKeyState","provTelState","provWifiState","provMqttState","provNameState"/.test(html));
ok("disconnected override returns early (no stale state derivation)",
  /sync status unknown\."\);\s*\}\);\s*return;/.test(html));
ok("CHANGELOG has 0.4.93 entry",
  /\{v:"0.4.93"/.test(html));

/* --- 0.4.94: Muse command stream visible in all modes --- */
ok("_sealPeekCmd helper exists",
  /function _sealPeekCmd\(b64\)/.test(html));
/* --- 0.4.95: full command specificity in Muse stream --- */
ok("_museCmdLabel helper exists",
  /function _museCmdLabel\(cmd\)/.test(html));
ok("_museCmdLabel shows dbg sub-command",
  /cmd\.t==="dbg"&&typeof cmd\.cmd==="string"\)return "dbg "\+cmd\.cmd/.test(html));
ok("bridged sealed_fwd path uses _museCmdLabel",
  /_plbl=_pc\?_museCmdLabel\(_pc\)/.test(html));
ok("native mode uses _museCmdLabel",
  /_nt=_museCmdLabel\(_npc\)/.test(html));
ok("page-side Muse dispatch uses _museCmdLabel",
  /MUSE → "\+_museCmdLabel\(cmd\)\+" \(page\)/.test(html));
ok("CHANGELOG has 0.4.95 entry",
  /\{v:"0.4.95"/.test(html));
/* --- 0.4.96: SEAL status popover --- */
ok("sealSub is keyboard-accessible button",
  /id="sealSub"[^>]*role="button"[^>]*tabindex="0"/.test(html));
ok("sealPopover markup exists",
  /id="sealPopover"[^>]*role="dialog"/.test(html));
ok("sealPopoverBackdrop exists",
  /id="sealPopoverBackdrop"[^>]*class="popover-backdrop"/.test(html));
ok("sealShowPopover positions near sealSub",
  /function sealShowPopover\(\)\{[^}]*sealSub[^}]*getBoundingClientRect/.test(html));
ok("sealPopRender shows page mode",
  /function sealPopRender\(\)[\s\S]{0,400}Page mode/.test(html));
ok("sealPopRender shows device seal floor",
  /Device seal floor/.test(html));
ok("sealPopRender shows telemetry seed status",
  /Telemetry seed/.test(html));
ok("sealSub click toggles popover",
  /sealSub[^}]*addEventListener\("click"/.test(html));
ok("Escape dismisses seal popover",
  /addEventListener\("keydown",function\(e\)\{if\(e\.key==="Escape"\)sealHidePopover/.test(html));
ok("CHANGELOG has 0.4.96 entry",
  /\{v:"0.4.96"/.test(html));

console.log(`\nLAYOUT: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
