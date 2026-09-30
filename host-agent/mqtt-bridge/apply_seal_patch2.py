#!/usr/bin/env python3
"""Phase 2 of the seal patch: Setup card, seal UI JS, seal_hw status, init."""
import sys

P = "/home/hatch/workspace/your_files/watcher-console/watcher-console.html"
MARKER = "WMS2-SEAL-PATCH-0.4.54"

html = open(P, encoding="utf-8").read()
assert MARKER in html, "phase 1 not applied"

def rep(old, new, count=1):
    global html
    assert html.count(old) == count, "expected %d of %r, found %d" % (count, old[:70], html.count(old))
    html = html.replace(old, new)

# 7. Setup card after the "Allow Muse to run commands" row.
anchor = '<button id="cmdToggle" class="toggle" role="switch" aria-checked="false" aria-label="Allow Muse to run commands on Watcher"><i></i></button></div>'
card = anchor + '''
      <div class="setup-row"><div><h2>Command authentication (WMS2 seal)</h2><p>Commands arriving on <b>&lt;prefix&gt;/command</b> are verified against a pinned Ed25519 commander key before they reach the serial dispatch. In grace mode unsigned commands are honored with a warning; strict mode drops them.</p><div class="seal-card"><div class="inline-fields"><input id="sealPk" placeholder="Commander public key (64 hex chars)" spellcheck="false" autocomplete="off"><button id="sealPin" class="secondary" type="button">Pin key</button></div><div class="seal-meta">Fingerprint <b id="sealFp">NONE</b> <span id="sealFpNote" class="small-note"></span></div><div class="inline-fields"><label class="allow-option"><input type="checkbox" id="sealStrict"> Strict mode \\u2014 drop unsigned commands</label><button id="sealHwReset" class="secondary" type="button">Reset replay counter</button></div><div class="seal-meta">Replay high-water <b id="sealHw">\\u2014</b> \\u00b7 Verified <b id="sealVerified">0</b> \\u00b7 Rejected <b id="sealRejected">0</b> (BAD-SIG <b id="sealBadSig">0</b>)</div><div id="sealLog" class="seal-log" aria-live="polite"></div></div></div></div>'''
rep(anchor, card)

# 8. seal_hw in the retained status publish (feeds the D4 commander resync).
rep('''mqttPublish(t.status,JSON.stringify({console:"online",page:CONSOLE_VERSION,at:new Date().toISOString(),fw:state.device.fw||null,cmd:state.cmdAllowed,unattended:state.unattendedFlash,fiducials:twinFiducialsInfo()}),1)''',
'''var _st={console:"online",page:CONSOLE_VERSION,at:new Date().toISOString(),fw:state.device.fw||null,cmd:state.cmdAllowed,unattended:state.unattendedFlash,fiducials:twinFiducialsInfo()};try{var _hw=SEAL.getHighWater();if(_hw&&Object.keys(_hw).length)_st.seal_hw=_hw}catch(_se){}mqttPublish(t.status,JSON.stringify(_st),1)''')

# 9. seal UI JS block before the final init lines.
seal_js = '''
  /* ---- WMS2 seal UI (%s) ---- */
  var sealNativeMqtt=false; /* flipped when the device reports native MQTT */
  var sealUI={verified:0,rejected:0,badsig:0,grace:0,log:[],alertT:0};
  var SEAL_FP_KEY="watcherSealFp";
  function sealFpCached(){try{return localStorage.getItem(SEAL_FP_KEY)||null}catch(_e){return null}}
  function sealCacheFp(fp){try{if(fp)localStorage.setItem(SEAL_FP_KEY,fp);else localStorage.removeItem(SEAL_FP_KEY)}catch(_e){}}
  function sealFingerprintHex(pkHex,cb){
    var cached=sealFpCached();if(cached){cb(cached);return}
    function done(fp){sealCacheFp(fp);cb(fp)}
    try{
      if(window.crypto&&crypto.subtle&&/^[0-9a-f]{64}$/.test(pkHex)){
        var bytes=new Uint8Array(32);
        for(var i=0;i<32;i++)bytes[i]=parseInt(pkHex.substr(i*2,2),16);
        crypto.subtle.digest("SHA-256",bytes).then(function(d){
          var h=new Uint8Array(d),s="";
          for(var j=0;j<h.length;j++)s+=("0"+h[j].toString(16)).slice(-2);
          done(s.slice(0,16));
        }).catch(function(){done(null)});
        return;
      }
    }catch(_e){}
    done(null);
  }
  function sealRenderPill(){
    var pill=$("sealPill");if(!pill||typeof SEAL==="undefined")return;
    var pk=SEAL.getPk(),strict=SEAL.isStrict();
    var cls="seal-pill "+(!pk?"none":strict?"strict":"grace");
    if(sealUI.alertT&&Date.now()<sealUI.alertT)cls+=" alert";
    pill.className=cls;
    var fp=sealFpCached();
    $("sealPillText").textContent="WMS2 \\u00b7 Ed25519 \\u00b7 fp "+(fp?fp.slice(0,4)+"\\u2026":"NONE")+" \\u00b7 "+(sealNativeMqtt?"native":"bridged");
    pill.title="Seal: "+(!pk?"NO KEY \\u2014 paste a commander key under Setup \\u2192 Command authentication":strict?"STRICT":"GRACE")+" \\u00b7 verified "+sealUI.verified+" \\u00b7 rejected "+sealUI.rejected+" (BAD-SIG "+sealUI.badsig+")";
  }
  function sealRenderCard(){
    if(typeof SEAL==="undefined")return;
    var hw=SEAL.getHighWater(),ks=Object.keys(hw||{});
    var hwEl=$("sealHw");if(hwEl)hwEl.textContent=ks.length?ks.map(function(k){return k+": "+hw[k]}).join(", "):"\\u2014";
    var v=$("sealVerified");if(v)v.textContent=String(sealUI.verified);
    var r=$("sealRejected");if(r)r.textContent=String(sealUI.rejected);
    var b=$("sealBadSig");if(b)b.textContent=String(sealUI.badsig);
    var pk=SEAL.getPk(),fp=sealFpCached();
    var f=$("sealFp");if(f)f.textContent=fp||(pk?"computing\\u2026":"NONE");
    var fn=$("sealFpNote");if(fn)fn.textContent=!fp?"":fp==="c0ccdd20d4d6e89f"?"\\u2713 provisioned commander key":"\\u26a0 fingerprint does not match the provisioned key";
    var st=$("sealStrict");if(st){st.checked=SEAL.isStrict();st.disabled=!pk;st.title=pk?"Drop unsigned commands":"Pin a commander key first";}
    var log=$("sealLog");if(log)log.textContent=sealUI.log.join("\\n");
  }
  function sealTelemetryEvent(e){
    if(e.seal==="verified"){sealUI.verified++;sealUI.alertT=0;}
    else if(e.seal==="rejected"){
      sealUI.rejected++;
      if(e.rc==="BAD_SIG")sealUI.badsig++;
      sealUI.log.unshift(new Date().toLocaleTimeString()+" "+(e.log||e.rc||"rejected"));
      if(sealUI.log.length>20)sealUI.log.pop();
      sealUI.alertT=Date.now()+4000;
    }
    else if(e.seal==="grace"){sealUI.grace++;}
    queueTelemetry({raw:JSON.stringify({t:"seal_event",seal:e.seal,rc:e.rc||null,from:e.from||null,ctr:(e.ctr==null?null:e.ctr),log:String(e.log||"").slice(0,200)}),kind:"json",level:e.seal==="rejected"?"error":"info"});
    sealRenderPill();sealRenderCard();
  }
  function sealInit(){
    if(typeof SEAL==="undefined"){var p=$("sealPill");if(p)p.title="Seal: verifier failed to load";return}
    var pin=$("sealPin"),pkIn=$("sealPk");
    function curPk(){return SEAL.getPk()}
    if(pkIn&&curPk())pkIn.value=curPk();
    if(pin)pin.addEventListener("click",function(){
      var v=((pkIn&&pkIn.value)||"").trim().toLowerCase();
      if(!/^[0-9a-f]{64}$/.test(v)){toast("Key must be 64 hex characters",true);return}
      var cur=curPk();
      if(cur&&cur!==v&&!confirm("Replace the pinned commander key?\\n\\nThis changes who can command this device."))return;
      try{localStorage.setItem(SEAL._lsKeys.pk,v);sealCacheFp(null)}catch(_e){}
      if(pkIn)pkIn.value=v;
      addLine("SEAL: commander key pinned"+(cur&&cur!==v?" (replaced)":""));
      sealFingerprintHex(v,function(fp){
        if(!fp)toast("Key pinned, but the fingerprint could not be computed here",true);
        else if(fp!=="c0ccdd20d4d6e89f")toast("Key pinned \\u2014 fingerprint "+fp+" does NOT match the provisioned commander key",true);
        else toast("Commander key pinned \\u00b7 fp c0cc\\u2026");
        sealRenderPill();sealRenderCard();
      });
      sealRenderPill();sealRenderCard();
    });
    var st=$("sealStrict");
    if(st)st.addEventListener("change",function(){
      if(st.checked&&!curPk()){st.checked=false;toast("Pin a commander key before enabling strict mode",true);return}
      try{localStorage.setItem(SEAL._lsKeys.strict,st.checked?"1":"0")}catch(_e){}
      addLine("SEAL: strict mode "+(st.checked?"ON \\u2014 unsigned commands will be dropped":"OFF \\u2014 grace mode"));
      sealRenderPill();sealRenderCard();
    });
    var hwR=$("sealHwReset");
    if(hwR)hwR.addEventListener("click",function(){
      if(!confirm("Reset the seal replay high-water mark?\\n\\nDo this only if the host lost its command counter file."))return;
      SEAL.resetHighWater();
      addLine("SEAL: replay high-water reset by operator");
      sealRenderCard();
    });
    var pk0=curPk();
    if(pk0&&!sealFpCached())sealFingerprintHex(pk0,function(){sealRenderPill();sealRenderCard();});
    sealRenderPill();sealRenderCard();
  }
''' % MARKER
rep("  museStatusRender();museHistRender();", seal_js + "  sealInit();\n  museStatusRender();museHistRender();")

# 10. pre-line for the rejection log.
rep(".seal-log{max-height:120px;overflow:auto;font:500 11px var(--mono);color:var(--danger);display:flex;flex-direction:column;gap:2px}",
    ".seal-log{max-height:120px;overflow:auto;font:500 11px var(--mono);color:var(--danger);white-space:pre-line}")

open(P, "w", encoding="utf-8").write(html)
print("phase 2 ok: setup card, seal_hw, seal UI, init")
