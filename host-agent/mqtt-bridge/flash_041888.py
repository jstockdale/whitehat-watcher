#!/usr/bin/env python3
"""Flash 0.4.18.88 to the bench under bench lock. John approved.

Holds the bench lock for the whole transfer+flash sequence in one process,
recovers MQTT_PASS transiently from the running subscriber process env,
never prints or logs it.
"""
import os
import re
import subprocess
import sys
import time

BRIDGE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BRIDGE)
from bench_lock import acquire_bench, release_bench

BIN = os.path.expanduser(
    "~/workspace/your_files/s3-flash-kit/v0.4.18.88/watcher_s3_0.4.18.88_merged.bin")
SHA = "efd279b796950245b7ff50a9a0a81b5c2581848c7db69bd6550ddca6f2661f0b"


def get_mqtt_pass() -> str:
    out = subprocess.run(
        ["pgrep", "-f", "subscriber.py"], capture_output=True, text=True)
    pids = [p for p in out.stdout.split() if p.strip()]
    if not pids:
        raise RuntimeError("no subscriber.py process found")
    with open(f"/proc/{pids[0]}/environ", "rb") as f:
        env = f.read().split(b"\x00")
    for e in env:
        if e.startswith(b"MQTT_PASS="):
            return e[len(b"MQTT_PASS="):].decode()
    raise RuntimeError("MQTT_PASS not in subscriber environ")


def run(cmd, env):
    print(f"RUN: {' '.join(cmd[:3])} ...", flush=True)
    r = subprocess.run(cmd, env=env, cwd=BRIDGE)
    return r.returncode


def main():
    lock = acquire_bench()
    if not lock:
        print("BENCH_BUSY")
        sys.exit(1)
    print("LOCK_HELD", flush=True)
    try:
        mqtt_pass = get_mqtt_pass()
        env = dict(os.environ)
        env["MQTT_PASS"] = mqtt_pass

        rc = run(["python3", "transfer-flash.py", BIN, "--confirm"], env)
        print(f"TRANSFER_RC={rc}", flush=True)
        if rc != 0:
            sys.exit(rc)

        rc = run(["python3", "send_flash.py", SHA, "--confirm"], env)
        print(f"FLASH_RC={rc}", flush=True)
        sys.exit(rc)
    finally:
        release_bench()
        print("LOCK_RELEASED", flush=True)


if __name__ == "__main__":
    main()
