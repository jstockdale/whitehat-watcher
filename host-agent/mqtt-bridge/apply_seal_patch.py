#!/usr/bin/env python3
"""Apply the WMS2 seal patch to watcher-console.html -> v0.4.54.

Idempotent-ish: refuses to run twice (checks for SEAL_PATCH_MARKER).
Backs up the original to watcher-console.html.pre-0.4.54.bak.
"""
import re, sys

P = "/home/hatch/workspace/your_files/watcher-console/watcher-console.html"
D = "/home/hatch/workspace/projects/watcher-input-assistant/host-agent/mqtt-bridge/"
MARKER = "WMS2-SEAL-PATCH-0.4.54"

html = open(P, encoding="utf-8").read()
if MARKER in html:
    print("patch already applied; refusing")
    sys.exit(1)
open(P + ".pre-0.4.54.bak", "w", encoding="utf-8").write(html)

def rep(old, new, count=1):
    global html
    assert html.count(old) == count, "expected %d of %r, found %d" % (count, old[:70], html.count(old))
    html = html.replace(old, new)

# 1. version bump
rep('var CONSOLE_VERSION="0.4.53"', 'var CONSOLE_VERSION="0.4.54"')

# 2. noble crypto bundle as its own script block after the esptool data-uri script.
#    The esptool script is one giant line ending in '"></script>'.
bundle = open(D + "seal-crypto-bundle.js", encoding="utf-8").read().strip()
assert "</script" not in bundle.lower()
m = re.search(r'<script src="data:application/javascript;base64,[^"]*"></script>', html)
assert m, "esptool script tag not found"
bundle_tag = ('\n<script>\n/* %s: WMS2 seal crypto — @noble/curves (ed25519) +\n * @noble/hashes (blake2b), MIT. Sets window.watcherSealCrypto.\n * Page stays fully self-contained (no network fetch). */\n%s\n</script>'
              % (MARKER, bundle))
html = html[:m.end()] + bundle_tag + html[m.end():]

# 3. SEAL verifier module right after the CONSOLE_VERSION line.
draft = open(D + "page_seal_draft.js", encoding="utf-8").read().rstrip()
assert draft.startswith("/*")
seal_mod = ("\n/* %s: WMS2 command verifier (page_seal_draft.js, pinned).\n"
            " * Verifies sealed envelopes on watcher/command before dispatch. */\n%s\n"
            % (MARKER, draft))
rep('var CONSOLE_VERSION="0.4.54",PAGE_STARTED_AT=Date.now();',
    'var CONSOLE_VERSION="0.4.54",PAGE_STARTED_AT=Date.now();' + seal_mod)

# 4. truncation exemption (D5d): frame/audio chunk events bypass the 4000-char cap.
rep('state.telemetryQueue.push({raw:line.raw.slice(0,4000),',
    'state.telemetryQueue.push({raw:line.noTruncate?line.raw:line.raw.slice(0,4000),')
rep('if(state.telemetry)queueTelemetry({raw:JSON.stringify(j),kind:"json",level:"info"});',
    'if(state.telemetry)queueTelemetry({raw:JSON.stringify(j),kind:"json",level:"info",noTruncate:true});')
rep('if(line.json&&line.json.t==="audio")privacyAudioChunk(line.json);',
    'if(line.json&&line.json.t==="audio"){privacyAudioChunk(line.json);line.noTruncate=true;}')

# 5. seal gate at the top of the command-topic branch in mqpOnPublish.
rep('''    if(topic!==t.command)return;var msg;try{msg=JSON.parse(payload)}catch(_e){return}
    var cmd=msg&&msg.cmd,id=msg&&msg.id;''',
'''    if(topic!==t.command)return;
    /* WMS2 seal gate (%s): verify sealed envelopes before any command
     * parsing. Grace honors unsigned with a 1-in-64 warning; strict drops. */
    var _sg=SEAL.guard(payload,topic,function(l){addLine(l)},function(e){sealTelemetryEvent(e)});
    if(!_sg.ok)return;
    if(_sg.dup)return; /* QoS echo of an already-processed command: silent */
    var cmd=_sg.cmd,id=_sg.id;''' % MARKER)

# 6. topbar pill (after the MUSE pill) + CSS + 6-column grid.
rep('<span class="muse-pill clickable connecting" id="musePill"',
    '<span class="seal-pill none" id="sealPill" role="status" aria-label="Command seal status" title="Seal: no key pinned"><i></i><b id="sealPillText">WMS2 · Ed25519 · fp NONE · bridged</b></span>\n    <span class="muse-pill clickable connecting" id="musePill"')
rep('.topbar .ka-pill,.topbar .muse-pill,.topbar .fw-badge,.topbar .conn-stack{zoom:.8333}',
    '.topbar .ka-pill,.topbar .muse-pill,.topbar .seal-pill,.topbar .fw-badge,.topbar .conn-stack{zoom:.8333}')
rep('grid-template-columns:minmax(0,1fr) auto auto auto auto;',
    'grid-template-columns:minmax(0,1fr) auto auto auto auto auto;')
seal_css = '''
    .seal-pill{display:inline-flex;align-items:center;gap:7px;border:1px solid var(--border);border-radius:999px;padding:10px 14px;font:800 12px var(--mono);letter-spacing:.04em;color:var(--muted);background:color-mix(in srgb,var(--muted) 8%,transparent);white-space:nowrap}
    .seal-pill i{width:8px;height:8px;border-radius:50%;background:var(--muted);opacity:calc(.3 + .7*var(--breath,1));transform:scale(calc(.75 + .25*var(--breath,1)))}
    .seal-pill.grace{color:var(--amber);border-color:color-mix(in srgb,var(--amber) 55%,transparent);background:color-mix(in srgb,var(--amber) 10%,transparent)}
    .seal-pill.grace i{background:var(--amber)}
    .seal-pill.strict{color:var(--accent);border-color:color-mix(in srgb,var(--accent) 55%,transparent);background:color-mix(in srgb,var(--accent) 10%,transparent)}
    .seal-pill.strict i{background:var(--accent)}
    .seal-pill.alert{color:var(--danger);border-color:color-mix(in srgb,var(--danger) 65%,transparent);background:color-mix(in srgb,var(--danger) 12%,transparent)}
    .seal-pill.alert i{background:var(--danger);opacity:1;transform:scale(1)}
    .seal-card{display:flex;flex-direction:column;gap:8px;margin-top:8px}
    .seal-meta{font:500 12px var(--mono);color:var(--muted)}
    .seal-meta b{color:var(--text)}
    .seal-log{max-height:120px;overflow:auto;font:500 11px var(--mono);color:var(--danger);display:flex;flex-direction:column;gap:2px}
'''
rep('    .muse-pill[hidden]{display:none}', '    .muse-pill[hidden]{display:none}' + seal_css)

open(P, "w", encoding="utf-8").write(html)
print("phase 1 ok: version, bundle, module, truncation, gate, pill, css")
