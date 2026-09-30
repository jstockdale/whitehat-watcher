#!/usr/bin/env python3
"""Watcher keepalive — two-layer state-sync daemon (rewritten 2026-09-24).

Layer 1 — BLE link keepalive (invisible): the firmware sends zero-movement
HID reports every interval_s so the host keeps the BLE connection alive.
Always runs on its schedule; no pill, no indicator. Only control is the
interval (0 = disabled, default 60). This daemon ensures the firmware has
the configured interval on startup and re-asserts if the state goes stale.

Layer 2 — Host OS jiggler (pill-controlled): the firmware nudges the mouse
+N/-N HID units (net-zero) every interval to keep the host OS from
sleeping. The console's Keepalive pill toggles it. This daemon adopts the
retained watcher/jiggle state and never fights a fresh console choice.

Protocols (retained):
  watcher/keepalive: {"t":"keepalive_state","interval_s":N,"by":"console|bridge","at":iso}
  watcher/jiggle:    {"t":"jiggle_state","on":bool,"px":N,"interval_s":N,
                      "subpixel":bool,"by":"console|bridge","at":iso}

One-shot flags (bench work):
  --ble-interval N   set BLE keepalive interval (0=disable), then exit
  --jiggle-on        OS jiggler ON (publish + device cmd), then exit
  --jiggle-off       OS jiggler OFF (publish + device cmd), then exit
  --jiggle-px N      set jiggler pixel size (with --jiggle-on or alone)
  --jiggle-interval N set jiggler interval seconds

Run under run-keepalive.sh (supervisor): restarts this process if it exits.
Credentials are recovered from the running subscriber.py environment
(see recover_env in eyes_fast.py) — never stored here.
"""

import argparse
import json
import os
import signal
import ssl
import topic_acl
import sys
import time
import traceback
import uuid
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from eyes_fast import recover_env  # noqa: E402

HERE = Path(__file__).parent
LOG_PATH = HERE / "keepalive.log"
HB_PATH = Path("/tmp/watcher-keepalive.hb")
CONF_PATH = HERE / "keepalive.conf"

TOPIC_PREFIX = "watcher"
KA_TOPIC = f"{TOPIC_PREFIX}/keepalive"
JG_TOPIC = f"{TOPIC_PREFIX}/jiggle"
# Bridge-owned directions (single writer: this daemon / bridge CLIs).
# The page owns watcher/keepalive + watcher/jiggle; the bridge publishes
# its own claims here so each topic has exactly one writer.
KA_BRIDGE_TOPIC = f"{TOPIC_PREFIX}/keepalive_bridge"
JG_BRIDGE_TOPIC = f"{TOPIC_PREFIX}/jiggle_bridge"
CMD_TOPIC = f"{TOPIC_PREFIX}/command"

DEFAULT_BLE_INTERVAL = 60   # seconds; 0 = disabled
DEFAULT_JG_PX = 1
DEFAULT_JG_INTERVAL = 60

STALE_AFTER_S = 15 * 60      # no state this old -> nobody managing
WATCHDOG_INTERVAL_S = 30     # how often the watchdog checks
STARTUP_GRACE_S = 4          # wait for retained state on startup



def _pub(client, user, topic, payload, **kw):
    """publish_checked with daemon semantics: a topic the ACL doesn't grant
    is BLOCKED with a loud ACL-DENY banner (log + stderr) instead of being
    silently dropped by the broker. Never raises -- the daemon must stay up."""
    return topic_acl.publish_checked(client, topic, payload, user=user,
                                     on_deny="skip", log=log, **kw)

def log(msg):
    line = f"{time.strftime('%F %T')} {msg}"
    print(line, flush=True)
    try:
        with open(LOG_PATH, "a") as f:
            f.write(line + "\n")
    except Exception:
        pass


def write_heartbeat(info):
    try:
        tmp = HB_PATH.with_suffix(".hb.tmp")
        tmp.write_text(json.dumps(info))
        os.replace(tmp, HB_PATH)
    except Exception:
        try:
            HB_PATH.touch()
        except Exception:
            pass


def load_conf():
    """Read keepalive.conf (KEY=VALUE lines); missing file -> defaults."""
    conf = {}
    try:
        for line in CONF_PATH.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            conf[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return conf


class TwoLayerSync:
    def __init__(self):
        env = recover_env()
        self.host = env.get("MQTT_HOST", "mqtt.offx1.com")
        self.port = int(env.get("MQTT_PORT", "8883"))
        self.user = env.get("MQTT_USER", "muse")
        self.passwd = env["MQTT_PASS"]
        conf = load_conf()
        self.ble_interval = int(conf.get("BLE_INTERVAL",
                                        DEFAULT_BLE_INTERVAL))
        self.ka_last = None  # most recent keepalive_state
        self.jg_last = None  # most recent jiggle_state

        import paho.mqtt.client as mqtt
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  client_id=f"muse-keepalive-{os.getpid()}")
        self.client.username_pw_set(self.user, self.passwd)
        self.client.tls_set(cert_reqs=ssl.CERT_REQUIRED,
                            tls_version=ssl.PROTOCOL_TLS_CLIENT)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.connect(self.host, self.port, keepalive=30)
        self.client.loop_start()
        time.sleep(1.0)

    def _on_connect(self, _c, _u, _f, rc, _p=None):
        if rc == 0:
            self.client.subscribe(KA_TOPIC, qos=0)
            self.client.subscribe(JG_TOPIC, qos=0)
            self.client.subscribe(KA_BRIDGE_TOPIC, qos=0)
            self.client.subscribe(JG_BRIDGE_TOPIC, qos=0)
            log(f"subscribed: {KA_TOPIC}, {JG_TOPIC}, "
                f"{KA_BRIDGE_TOPIC}, {JG_BRIDGE_TOPIC}")
        else:
            log(f"connect failed: rc={rc}")

    def _on_message(self, _c, _u, msg):
        try:
            p = json.loads(msg.payload.decode("utf-8", "replace"))
        except Exception:
            return
        if not isinstance(p, dict):
            return
        now = time.time()
        if msg.topic in (KA_TOPIC, KA_BRIDGE_TOPIC) \
                and p.get("t") == "keepalive_state":
            self.ka_last = {"interval_s": int(p.get("interval_s", 60)),
                            "by": p.get("by", "?"),
                            "at": p.get("at", "?"),
                            "ts": now,
                            "retained": bool(msg.retain)}
            log(f"ble-ka state: interval={self.ka_last['interval_s']}s "
                f"by={self.ka_last['by']}")
        elif msg.topic in (JG_TOPIC, JG_BRIDGE_TOPIC) \
                and p.get("t") == "jiggle_state":
            self.jg_last = {"on": bool(p.get("on")),
                            "px": int(p.get("px", DEFAULT_JG_PX)),
                            "interval_s": int(p.get("interval_s",
                                                   DEFAULT_JG_INTERVAL)),
                            "subpixel": bool(p.get("subpixel")),
                            "by": p.get("by", "?"),
                            "at": p.get("at", "?"),
                            "ts": now,
                            "retained": bool(msg.retain)}
            log(f"jiggle state: on={self.jg_last['on']} "
                f"by={self.jg_last['by']}")

    # ---- BLE keepalive (layer 1) ----
    def publish_ble(self, interval_s, by="bridge"):
        payload = {"t": "keepalive_state", "interval_s": int(interval_s),
                   "by": by,
                   "at": datetime.now(timezone.utc).isoformat()}
        _pub(self.client, self.user, KA_BRIDGE_TOPIC, json.dumps(payload), qos=0, retain=True)
        self.ka_last = {"interval_s": int(interval_s), "by": by,
                        "at": payload["at"], "ts": time.time(),
                        "retained": False}
        log(f"published ble-ka: interval={interval_s}s by={by}")

    def send_ble_device(self, interval_s):
        cmd = {"t": "keepalive", "interval_s": int(interval_s)}
        msg = {"cmd": cmd, "id": uuid.uuid4().hex[:8]}
        _pub(self.client, self.user, CMD_TOPIC, json.dumps(msg), qos=0)
        log(f"device cmd {msg['id']}: {json.dumps(cmd)} "
            f"(needs console cmd-channel + serial)")

    def ensure_ble(self):
        """Make sure the firmware has our BLE interval (startup + watchdog)."""
        self.publish_ble(self.ble_interval)
        self.send_ble_device(self.ble_interval)

    def ble_is_fresh(self):
        return (self.ka_last is not None
                and time.time() - self.ka_last["ts"] < STALE_AFTER_S)

    # ---- OS jiggler (layer 2) ----
    def publish_jiggle(self, on, px, interval_s, subpixel, by="bridge"):
        payload = {"t": "jiggle_state", "on": bool(on), "px": int(px),
                   "interval_s": int(interval_s),
                   "subpixel": bool(subpixel), "by": by,
                   "at": datetime.now(timezone.utc).isoformat()}
        _pub(self.client, self.user, JG_BRIDGE_TOPIC, json.dumps(payload), qos=0, retain=True)
        self.jg_last = {"on": bool(on), "px": int(px),
                        "interval_s": int(interval_s),
                        "subpixel": bool(subpixel), "by": by,
                        "at": payload["at"], "ts": time.time(),
                        "retained": False}
        log(f"published jiggle: on={on} px={px} every={interval_s}s "
            f"subpixel={subpixel} by={by}")

    def send_jiggle_device(self, on, px, interval_s, subpixel):
        cmd = {"t": "jiggle", "on": 1 if on else 0, "px": int(px),
               "interval_s": int(interval_s),
               "subpixel": 1 if subpixel else 0}
        msg = {"cmd": cmd, "id": uuid.uuid4().hex[:8]}
        _pub(self.client, self.user, CMD_TOPIC, json.dumps(msg), qos=0)
        log(f"device cmd {msg['id']}: {json.dumps(cmd)} "
            f"(needs console cmd-channel + serial)")

    def jiggle_is_fresh(self):
        return (self.jg_last is not None
                and time.time() - self.jg_last["ts"] < STALE_AFTER_S)

    def close(self):
        try:
            self.client.loop_stop()
            self.client.disconnect()
        except Exception:
            pass


def one_shot(args):
    ka = TwoLayerSync()
    try:
        if args.ble_interval is not None:
            ka.ble_interval = args.ble_interval
            ka.publish_ble(args.ble_interval)
            ka.send_ble_device(args.ble_interval)
        if args.jiggle_on or args.jiggle_off or args.jiggle_px is not None \
                or args.jiggle_interval is not None:
            cur = ka.jg_last or {}
            on = True if args.jiggle_on else \
                False if args.jiggle_off else cur.get("on", False)
            px = args.jiggle_px if args.jiggle_px is not None \
                else cur.get("px", DEFAULT_JG_PX)
            iv = args.jiggle_interval if args.jiggle_interval is not None \
                else cur.get("interval_s", DEFAULT_JG_INTERVAL)
            sub = cur.get("subpixel", False)
            ka.publish_jiggle(on, px, iv, sub)
            ka.send_jiggle_device(on, px, iv, sub)
        time.sleep(3)
    finally:
        ka.close()


def daemon():
    ka = TwoLayerSync()
    stop = {"flag": False}

    def _sig(_n, _f):
        stop["flag"] = True

    signal.signal(signal.SIGTERM, _sig)
    signal.signal(signal.SIGINT, _sig)

    log("two-layer keepalive sync starting "
        f"(ble interval={ka.ble_interval}s)")
    time.sleep(STARTUP_GRACE_S)

    # Layer 1: adopt retained BLE state if fresh and sane; otherwise assert
    # our configured interval (takeover).
    if ka.ble_is_fresh():
        log(f"adopting BLE state (by {ka.ka_last['by']}) — staying out of the way")
    else:
        log("no fresh BLE state — asserting configured interval (takeover)")
        ka.ensure_ble()

    # Layer 2: never drive the jiggler from here — the console pill owns it.
    # Just adopt whatever retained state exists so the heartbeat reflects it.
    if ka.jiggle_is_fresh():
        log(f"adopting jiggle state (by {ka.jg_last['by']})")
    else:
        log("no jiggle state yet — pill owns it, staying out of the way")

    cycle = 0
    try:
        while not stop["flag"]:
            cycle += 1
            try:
                # Watchdog only guards layer 1 (invisible infra). Layer 2 is
                # user-controlled; we never override the pill.
                if not ka.ble_is_fresh():
                    log("BLE state stale — re-asserting interval")
                    ka.ensure_ble()
                write_heartbeat({"ts": time.strftime("%F %T"),
                                 "cycle": cycle,
                                 "ble_interval": ka.ble_interval,
                                 "ka_last": ka.ka_last,
                                 "jg_last": ka.jg_last,
                                 "ble_fresh": ka.ble_is_fresh(),
                                 "jg_fresh": ka.jiggle_is_fresh()})
            except Exception:
                log("WATCHDOG FAILED:")
                traceback.print_exc()
            for _ in range(WATCHDOG_INTERVAL_S * 2):
                if stop["flag"]:
                    break
                time.sleep(0.5)
    finally:
        # Graceful shutdown: leave BLE keepalive running (invisible infra);
        # never touch the jiggler on the way out.
        try:
            ka.publish_ble(ka.ble_interval)
            time.sleep(1)
        except Exception:
            pass
        ka.close()
        log("stopped")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ble-interval", type=int, default=None,
                    help="one-shot: set BLE keepalive interval seconds "
                         "(0=disable), then exit")
    ap.add_argument("--jiggle-on", action="store_true",
                    help="one-shot: OS jiggler ON, then exit")
    ap.add_argument("--jiggle-off", action="store_true",
                    help="one-shot: OS jiggler OFF, then exit")
    ap.add_argument("--jiggle-px", type=int, default=None,
                    help="one-shot: set jiggler pixel size")
    ap.add_argument("--jiggle-interval", type=int, default=None,
                    help="one-shot: set jiggler interval seconds")
    # Back-compat: old flags accepted but ignored.
    ap.add_argument("--on", action="store_true")
    ap.add_argument("--off", action="store_true")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--interval", type=int, default=90)
    ap.add_argument("--practice", action="store_true")
    ap.add_argument("--cycles", type=int, default=0)
    args = ap.parse_args()

    if args.on or args.off or args.release:
        log("note: --on/--off/--release are obsolete; "
            "use --jiggle-on/--jiggle-off for the OS jiggler")

    if args.ble_interval is not None or args.jiggle_on or args.jiggle_off \
            or args.jiggle_px is not None or args.jiggle_interval is not None:
        one_shot(args)
    else:
        if args.interval != 90 or args.practice or args.cycles:
            log("note: --interval/--practice/--cycles are obsolete; "
                "running as two-layer sync daemon")
        daemon()


if __name__ == "__main__":
    main()
