#!/usr/bin/env python3
"""apply_tel_patch.py — D5b page integration for watcher-console.html.

1. Inlines seal-aead-bundle.js after the noble crypto bundle.
2. Inlines page_tel_seal.js after the SEAL verifier module.
3. Hooks queueTelemetry: media lines (noTruncate) are sealed via
   TEL.maybeSealRaw BEFORE any truncation; sealed lines never truncate.
4. Adds the telemetry-seed provisioning card to the Setup tab + its JS.
5. Bumps CONSOLE_VERSION 0.4.54 -> 0.4.55.

Idempotent: refuses to run if the D5b markers are already present.
"""
import re, sys

PAGE = "/home/hatch/workspace/your_files/watcher-console/watcher-console.html"
BRIDGE = "/home/hatch/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge"
MARK = "WMS2-TEL-PATCH-0.4.55"

s = open(PAGE, encoding="utf-8").read()
if MARK in s:
    print("D5b patch already applied; refusing to double-apply.")
    sys.exit(1)

aead = open(f"{BRIDGE}/seal-aead-bundle.js", encoding="utf-8").read().rstrip("\n")
tel = open(f"{BRIDGE}/page_tel_seal.js", encoding="utf-8").read().rstrip("\n")

# 1. AEAD bundle after the noble crypto bundle (unique closing anchor)
anchor1 = "window.watcherSealCrypto={ed25519:mn,blake2b:wn};})();"
assert s.count(anchor1) == 1, "crypto bundle anchor not unique/found"
aead_block = ("\n/* %s: XChaCha20-Poly1305 for kind-1 ENC telemetry "
              "(seal-aead-bundle.js, pinned). */\n%s\n" % (MARK, aead))
s = s.replace(anchor1, anchor1 + aead_block, 1)

# 2. page_tel_seal.js after the SEAL module's closing `})();` (line 542)
#    Anchor: the seal-gate comment that follows the module.
anchor2 = "/* WMS2 seal gate (WMS2-SEAL-PATCH-0.4.54)"
assert s.count(anchor2) == 1, "seal gate anchor not unique/found"
tel_block = ("\n/* %s: kind-1 ENC telemetry sealer (page_tel_seal.js, pinned). */\n%s\n"
             % (MARK, tel))
s = s.replace(anchor2, tel_block + anchor2, 1)

# 3. queueTelemetry hook: seal media lines before truncation.
old_qt = ("function queueTelemetry(line){if(!state.telemetry)return;"
          "state.telemetryQueue.push({raw:line.noTruncate?line.raw:line.raw.slice(0,4000),"
          "kind:line.kind===\"json\"?\"reply\":\"log\",level:line.level});")
assert s.count(old_qt) == 1, "queueTelemetry anchor not unique/found"
new_qt = ("function queueTelemetry(line){if(!state.telemetry)return;"
          "var _qraw=line.raw,_sealed=false;"
          "/* %s: seal frame/audio lines BEFORE truncation; sealed lines never truncate. */"
          "if(line.noTruncate&&typeof TEL!==\"undefined\"){"
          "try{var _ts=TEL.maybeSealRaw(line.raw,mqttTopics().telemetry);"
          "if(_ts.sealed){_qraw=_ts.sealed;_sealed=true;}"
          "else if(_ts.plain){state.telPlainWarn=(state.telPlainWarn||0)+1;"
          "if(state.telPlainWarn%%64===1)addLine(JSON.stringify({t:\"tel_enc_warn\","
          "msg:\"telemetry ENC unavailable (\"+(_ts.noSeed?\"no seed provisioned\":\"crypto error\")+\") "
          "— media line sent PLAINTEXT\"}));}}catch(_te){}}"
          "state.telemetryQueue.push({raw:_sealed?_qraw:(line.noTruncate?line.raw:line.raw.slice(0,4000)),"
          "kind:line.kind===\"json\"?\"reply\":\"log\",level:line.level});" % MARK)
s = s.replace(old_qt, new_qt, 1)

# 4a. Setup tab card: telemetry seed provisioning (after the seal card row).
old_card = '<div id="sealLog" class="seal-log" aria-live="polite"></div></div></div></div>'
assert s.count(old_card) == 1, "seal card anchor not unique/found"
tel_card = (
    '<div id="sealLog" class="seal-log" aria-live="polite"></div></div></div>'
    '<div class="setup-row"><div><h2>Telemetry encryption (WMS2 kind-1 ENC)</h2>'
    '<p>Frame and audio chunks are sealed with XChaCha20-Poly1305 before they leave '
    'this page. One 32-byte seed derives the MAC and AEAD keys; it never leaves this '
    'browser (localStorage, this origin only) and is never displayed or logged.</p>'
    '<div class="seal-card">'
    '<div class="seal-meta">Seed fingerprint <b id="telFp">NONE</b> '
    '<span id="telFpNote" class="small-note"></span></div>'
    '<div class="inline-fields">'
    '<input id="telSeed" type="password" placeholder="Telemetry seed (64 hex chars)" '
    'spellcheck="false" autocomplete="off" style="font-family:monospace">'
    '<button id="telProvision" class="secondary" type="button">Provision seed</button>'
    '<button id="telClear" class="secondary" type="button">Clear seed</button>'
    '</div>'
    '<div class="seal-meta">Sealed chunks <b id="telSealed">0</b> · '
    'Plaintext media lines <b id="telPlain">0</b></div>'
    '</div></div></div>')
s = s.replace(old_card, tel_card, 1)

# 4b. Setup JS for the seed card (after the seal UI section marker).
anchor3 = "/* ---- WMS2 seal UI (WMS2-SEAL-PATCH-0.4.54) ---- */"
assert s.count(anchor3) == 1, "seal UI anchor not unique/found"
tel_ui = anchor3 + """
  /* ---- WMS2 telemetry seed UI (WMS2-TEL-PATCH-0.4.55) ---- */
  function telSealRefresh(){
    if(typeof TEL==="undefined")return;
    var fp=null;try{fp=TEL.fingerprint()}catch(_e){}
    var b=$("telFp");if(b)b.textContent=fp||"NONE";
    var n=$("telFpNote");if(n)n.textContent=fp?"provisioned · keys derived locally":"not provisioned — media lines go plaintext (warned 1-in-64)";
    var sc=$("telSealed");if(sc)sc.textContent=String(state.telSealedCount||0);
    var pl=$("telPlain");if(pl)pl.textContent=String(state.telPlainWarn||0);
  }
  (function(){
    var pv=$("telProvision");if(pv)pv.addEventListener("click",function(){
      if(typeof TEL==="undefined"){addLine("telemetry seal module not loaded");return}
      var inp=$("telSeed"),hex=inp?inp.value.trim():"";
      if(!/^[0-9a-fA-F]{64}$/.test(hex)){addLine("telemetry seed must be 64 hex chars");return}
      try{
        if(TEL.hasSeed()&&!confirm("Replace the existing telemetry seed? Already-sealed chunks stay sealed under the old key."))return;
        var fp=TEL.setSeedHex(hex);
        if(inp)inp.value="";
        addLine("telemetry seed provisioned · fp "+fp);
      }catch(e){addLine("seed provisioning failed: "+(e&&e.message||e))}
      telSealRefresh();
    });
    var cl=$("telClear");if(cl)cl.addEventListener("click",function(){
      if(typeof TEL==="undefined")return;
      if(!TEL.hasSeed())return;
      if(!confirm("Clear the telemetry seed? Sealing stops until a new seed is provisioned."))return;
      TEL.clearSeed();addLine("telemetry seed cleared");telSealRefresh();
    });
  })();"""
s = s.replace(anchor3, tel_ui, 1)

# count sealed chunks for the UI (in the queueTelemetry hook)
s = s.replace("if(_ts.sealed){_qraw=_ts.sealed;_sealed=true;}",
              "if(_ts.sealed){_qraw=_ts.sealed;_sealed=true;state.telSealedCount=(state.telSealedCount||0)+1;}", 1)

# refresh the seed card after init (hook into sealPill refresh if present)
anchor4 = 'function sealPill(){'
if s.count(anchor4) == 1:
    s = s.replace(anchor4,
                  'function telSealRefreshSafe(){try{if(typeof telSealRefresh!=="undefined")telSealRefresh()}catch(_e){}}\n' + anchor4, 1)

# 5. version bump
assert s.count('var CONSOLE_VERSION="0.4.54"') == 1
s = s.replace('var CONSOLE_VERSION="0.4.54"', 'var CONSOLE_VERSION="0.4.55"', 1)

open(PAGE, "w", encoding="utf-8").write(s)
print("D5b page patch applied; version -> 0.4.55")
