#!/usr/bin/env python3
"""Watcher MQTT bridge subscriber (Muse side).

Connects to John's Mosquitto broker over TLS, subscribes to the Watcher
uplink topics, and prints/logs telemetry batches. Credentials come from
environment variables -- never hardcoded:

    export MQTT_HOST=mqtt.offx1.com
    export MQTT_PORT=8883
    export MQTT_USER=muse
    export MQTT_PASS='...'

Topics (contract agreed 2026-09-23):
    watcher/telemetry  console -> Muse : {"device","fw","events":[{raw,kind,level}]}
    watcher/status     console -> Muse : retained {"console":"online|offline"}
    watcher/command    Muse -> console : {"id","cmd"}  (downlink, later)
"""

import atexit
import base64
import json
import os
import signal
import ssl
import sys
import threading
import time

import paho.mqtt.client as mqtt

from audio_handler import handle_device_message
from snaptest_handler import handle_snaptest_message
import topic_acl

# D5b: kind-1 ENC telemetry decryption. The 32B telemetry seed is NEVER
# logged; only its fingerprint is. Seed sources: ~/.config/watcher/tel_seed
# (64 hex chars, mode 0600) or WATCHER_TEL_SEED env.
try:
    import mqtt_seal as _mqs
except ImportError:  # pragma: no cover - seal module must ship with bridge
    _mqs = None

_TEL = {"mac": None, "aead": None, "fp": None, "replay": {}}


def _tel_keys_init():
    if _mqs is None:
        return
    try:
        seed, src = _mqs.load_tel_seed()
    except ValueError as e:
        log_line(f"SEAL: bad telemetry seed ({e}) - sealed chunks will DROP")
        return
    if seed is None:
        return
    k_mac, k_aead, fp = _mqs.derive_tel_keys(seed)
    _TEL["mac"], _TEL["aead"], _TEL["fp"] = k_mac, k_aead, fp
    log_line(f"SEAL: telemetry seed loaded ({src}) fp={fp}")


def _unwrap_sealed_chunk(raw, topic):
    """If raw is a {"t":"sealed_chunk","enc":...} wrapper, verify+decrypt.

    Returns (plaintext_raw_or_None, drop_bool). Plaintext is NEVER logged
    here — only named SEAL rejection lines.
    """
    try:
        o = json.loads(raw)
    except Exception:
        return raw, False
    if not isinstance(o, dict) or o.get("t") != "sealed_chunk":
        return raw, False
    enc = o.get("enc", "")
    if _TEL["aead"] is None:
        log_line("SEAL: NO TEL SEED for sealed chunk on '%s' - DROPPED "
                 "(provision ~/.config/watcher/tel_seed)" % topic)
        return None, True
    try:
        env = base64.b64decode(enc)
    except Exception:
        log_line("SEAL: SHORT sealed chunk on '%s' - DROPPED" % topic)
        return None, True
    # per-sender replay context (dev_id is inside the envelope; parse it
    # cheaply for the dict key — verify() re-parses authoritatively)
    dev_id = "?"
    if len(env) >= 22:
        dev_id = bytes(env[6:22]).split(b"\x00")[0].decode("ascii",
                                                           "replace") or "?"
    rp = _TEL["replay"].setdefault(dev_id, _mqs.ReplayCtx())
    rc, h, body, rlog = _mqs.verify(
        env, topic, key32=_TEL["mac"], aead_key=_TEL["aead"],
        now_ms=int(time.time() * 1000),
        replay=rp, policy=_mqs.Policy(ts_window_ms=120000, strict=False))
    if rlog:
        log_line(rlog)
    if rc != _mqs.OK and rc != _mqs.OK_DUP:
        return None, True
    if rc == _mqs.OK_DUP:
        return None, True  # silent dup: already handled
    return body.decode("utf-8", "replace"), False

HOST = os.environ.get("MQTT_HOST", "mqtt.offx1.com")
PORT = int(os.environ.get("MQTT_PORT", "8883"))
USER = os.environ.get("MQTT_USER", "muse")
PASS = os.environ.get("MQTT_PASS", "")

TOPICS = ["watcher/telemetry", "watcher/status", "watcher/audio",
          "watcher/bridge",
          "watcher/keepalive", "watcher/jiggle",
          "watcher/muse", "watcher/muse_pause",
          "watcher/muse_queue", "watcher/heartbeat",
          "watcher/console_heartbeat"]

LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "telemetry.log")


def log_line(msg: str) -> None:
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(LOG_PATH, "a") as f:
        f.write(line + "\n")


def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        log_line(f"connected to {HOST}:{PORT} as {USER}")
        for t in TOPICS:
            client.subscribe(t, qos=0)
            log_line(f"subscribed: {t}")
    else:
        log_line(f"connect failed: rc={reason_code}")
        sys.exit(1)


def on_disconnect(client, userdata, flags, reason_code, properties=None):
    # Diagnostic (2026-10-04): the bridge died silently 3x today with no
    # traceback. Log every broker-side disconnect so the next death leaves
    # evidence in subscriber.log. paho auto-reconnects unless the client
    # state is DISCONNECTING/DISCONNECTED (i.e. disconnect() was called).
    try:
        rc = reason_code.value if hasattr(reason_code, "value") else reason_code
    except Exception:
        rc = reason_code
    log_line(f"mqtt disconnected: rc={rc}")


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8", "replace"))
    except Exception:
        payload = {"_raw": msg.payload[:200].decode("utf-8", "replace")}
    if msg.topic == "watcher/telemetry":
        events = payload.get("events", [])
        fw = payload.get("fw", "?")
        log_line(f"[telemetry] fw={fw} events={len(events)}")
        # v2026-10-05: log ALL events, not just last 5. The [-5:] slice was
        # silently dropping Himax DIAG lines during bulk transfers (batches
        # of 8-9 events). John: keep everything until Muse accesses it.
        for e in events:
            raw = str(e.get("raw", ""))
            if '"sealed_chunk"' in raw[:60]:
                raw = "[sealed_chunk]"  # never log ciphertext into telemetry.log
            else:
                raw = raw[:255].replace("\n", " ")
            log_line(f"    [{e.get('level','?')}/{e.get('kind','?')}] {raw}")
        # PTT audio arrives as raw device JSON inside telemetry events
        # (current console); scan all events, not just the last 5.
        for e in events:
            raw = str(e.get("raw", ""))
            raw, drop = _unwrap_sealed_chunk(raw, msg.topic)
            if drop:
                continue
            wav = handle_device_message(raw)
            if wav:
                log_line(f"[audio] utterance saved: {wav}")
            # Snaptest images: auto-capture when bench is active
            img = handle_snaptest_message(raw)
            if img:
                log_line(f"[snaptest] image saved: {img}")
            # Catch BLE disconnects and other key events that might be
            # outside the last-5 window
            if "disconnected: reason=" in raw or "BLE_GAP_EVENT_DISCONNECT" in raw:
                log_line(f"    [ble-disconnect] {raw[:160]}")
    elif msg.topic == "watcher/audio":
        # Dedicated audio topic (preferred once the console routes t:audio
        # here instead of batching it into telemetry).
        body = msg.payload.decode("utf-8", "replace")
        body, drop = _unwrap_sealed_chunk(body, msg.topic)
        if drop:
            return
        wav = handle_device_message(body)
        if wav:
            log_line(f"[audio] utterance saved: {wav}")
        else:
            log_line(f"[audio] chunk ({len(msg.payload)}B)")
    elif msg.topic == "watcher/status":
        log_line(f"[status] retained={msg.retain} {payload}")
    elif msg.topic == "watcher/heartbeat":
        # Device heartbeat forwarded by the page (~1/min).
        if isinstance(payload, dict):
            log_line(f"[heartbeat] uptime_s={payload.get('uptime_s')} "
                     f"fw={payload.get('fw')} heap_free={payload.get('heap_free')}")
        else:
            log_line(f"[heartbeat] {str(payload)[:160]}")
    elif msg.topic == "watcher/console_heartbeat":
        # Page's own heartbeat (~1/min).
        if isinstance(payload, dict):
            log_line(f"[console_heartbeat] page={payload.get('page')} "
                     f"uptime_s={payload.get('uptime_s')} fw={payload.get('fw')}")
        else:
            log_line(f"[console_heartbeat] {str(payload)[:160]}")
    elif msg.topic == "watcher/muse_queue":
        if isinstance(payload, dict):
            t = payload.get("t", "")
            if t == "muse_queue_state":
                log_line(f"[muse_queue] depth={payload.get('depth')} "
                         f"retained={msg.retain}")
            elif t == "muse_queue_clear":
                log_line(f"[muse_queue] cleared by={payload.get('by')} "
                         f"retained={msg.retain}")
            else:
                log_line(f"[muse_queue] retained={msg.retain} {str(payload)[:200]}")
    elif msg.topic == "watcher/bridge":
        # Pings from the console arrive here; our own status messages too.
        if isinstance(payload, dict) and payload.get("t") == "bridge_ping":
            log_line(f"[bridge] ping received retained={msg.retain}")
            try:
                bridge_publish(client, BRIDGE_TOPIC, bridge_status_payload(),
                               retain=True)
                log_line("[bridge] ping answered")
            except Exception as e:
                log_line(f"[bridge] ping answer failed: {e}")
        # Ignore our own bridge_status messages (and other types).
    elif msg.topic == "watcher/muse_pause":
        if isinstance(payload, dict) and payload.get("t") == "muse_pause":
            paused = payload.get("paused", False)
            log_line(f"[muse_pause] paused={paused} by={payload.get('by')} "
                     f"retained={msg.retain}")
        else:
            log_line(f"[muse_pause] retained={msg.retain} {str(payload)[:200]}")
    elif msg.topic == "watcher/muse":
        if isinstance(payload, dict) and payload.get("t") == "muse_driving":
            log_line(f"[muse] driving={payload.get('driving')} "
                     f"retained={msg.retain}")
        else:
            log_line(f"[muse] retained={msg.retain} {str(payload)[:200]}")
    elif msg.topic == "watcher/keepalive":
        # Keepalive state sync (console <-> bridge). Retained so late
        # joiners converge: {"t":"keepalive_state",on,mode,by,at}.
        if isinstance(payload, dict) and payload.get("t") == "keepalive_state":
            log_line(f"[keepalive] on={payload.get('on')} "
                     f"mode={payload.get('mode')} by={payload.get('by')} "
                     f"retained={msg.retain}")
        else:
            log_line(f"[keepalive] retained={msg.retain} {str(payload)[:200]}")
    else:
        log_line(f"[{msg.topic}] {str(payload)[:200]}")


BRIDGE_TOPIC = "watcher/bridge"
IDLE_CHECKIN_INTERVAL = 60  # seconds; only when nothing else was published

# Timestamp of the last MQTT publish from this bridge (any topic).
_last_publish_at = 0.0
_publish_lock = threading.Lock()


def bridge_publish(client: mqtt.Client, topic: str, payload: str,
                   retain: bool = False) -> None:
    """Publish and record the timestamp (drives the idle check-in).

    The topic is pre-checked against the broker ACL (topic_acl): a topic
    the ACL doesn't grant is BLOCKED with a loud ACL-DENY banner instead
    of being silently dropped by the broker. on_deny="skip" -- the bridge
    must never die over a bad topic; the banner is the alarm.
    """
    global _last_publish_at
    try:
        # ACL pre-check: fail loudly, never silently drop.
        if not topic_acl.publish_checked(client, topic, payload, qos=0,
                                         retain=retain, on_deny="skip",
                                         log=log_line):
            return
        with _publish_lock:
            _last_publish_at = time.time()
    except Exception as e:
        log_line(f"[bridge] publish to {topic} failed: {e}")


def bridge_status_payload() -> str:
    return json.dumps({
        "t": "bridge_status",
        "at": int(time.time()),
        "pid": os.getpid(),
    })


def bridge_loop(client: mqtt.Client) -> None:
    """Answer pings immediately; send an idle check-in every 60s only when
    the bridge hasn't published anything else (quiet periods)."""
    while True:
        time.sleep(10)
        with _publish_lock:
            idle_for = time.time() - _last_publish_at
        if idle_for >= IDLE_CHECKIN_INTERVAL:
            bridge_publish(client, BRIDGE_TOPIC, bridge_status_payload(),
                           retain=True)
            log_line("[bridge] idle check-in published")


def main() -> None:
    if not PASS:
        print("MQTT_PASS is not set", file=sys.stderr)
        sys.exit(2)
    # Exit diagnostics (prepared 2026-10-16): 109 of 150 rc=128
    # disconnects since Oct 5 are the dying process's last log line,
    # with zero tracebacks, zero "loop_forever returned" lines and
    # zero "connect failed" lines in subscriber.log. Log every
    # remaining way out so the next death names its cause: a signal
    # (external kill), SystemExit, or atexit with neither (clean
    # interpreter teardown).
    def _log_exit(reason: str) -> None:
        try:
            log_line(f"[exit-diag] process exiting: {reason}")
        except Exception:
            pass

    def _on_signal(signum, frame):
        _log_exit(f"signal {signal.Signals(signum).name} ({signum})")
        # Restore the default disposition and re-deliver, so exit
        # semantics and the status the watchdog sees do not change.
        signal.signal(signum, signal.SIG_DFL)
        os.kill(os.getpid(), signum)

    for _sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(_sig, _on_signal)
    atexit.register(_log_exit, "atexit (no signal, no exception logged)")
    _tel_keys_init()  # D5b: load telemetry seed (fp logged, seed never)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id="muse-bridge-1")
    client.username_pw_set(USER, PASS)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                   tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    log_line(f"connecting to {HOST}:{PORT} ...")
    client.connect(HOST, PORT, keepalive=30)
    # Bridge status thread: answers pings, idle check-in every 60s
    bridge_thread = threading.Thread(target=bridge_loop, args=(client,),
                                     daemon=True)
    bridge_thread.start()
    log_line(f"[bridge] status+ping on {BRIDGE_TOPIC}; "
             f"idle check-in every {IDLE_CHECKIN_INTERVAL}s")
    try:
        rc = client.loop_forever()
        # loop_forever() returning (rather than raising) means the main
        # thread is exiting and the process will die with no traceback —
        # log it so a silent death becomes a diagnosed one.
        log_line(f"loop_forever returned rc={rc}; bridge exiting")
    except BaseException as e:
        # BaseException, not Exception: also catches SystemExit (e.g.
        # a sys.exit() inside a paho callback) and KeyboardInterrupt,
        # both of which currently leave no line in the log at all.
        log_line(f"loop_forever raised {type(e).__name__}: {e!r}; "
                 f"bridge exiting")
        raise


if __name__ == "__main__":
    main()
