#!/usr/bin/env python3
"""DEPRECATED wrapper — do not use directly.

send_cmd.py used to publish UNSEALED commands to watcher/command.
That was a security hole: in strict mode the seal gate drops unsealed
messages ("SEAL: UNSEALED msg on 'watcher/command' - DROPPED"), so the
command silently never reached the device.

This script now execs send_command.py — the single canonical sender —
with sealing ENABLED by default. The only supported flags are:

    --seal      (default) WMS2/Ed25519-sealed envelope
    --no-seal   DANGER: unsigned. ONLY for deliberately testing that
                strict mode rejects unsealed traffic.

The old convenience of reading the broker password from
~/.config/watcher-mqtt/mqtt_pass is preserved: if MQTT_PASS is not
already in the environment, it is loaded from that file before
exec'ing send_command.py.

New code MUST call send_command.py directly. See TESTING_PROTOCOL.md.
"""
import os
import sys

BRIDGE_DIR = os.path.dirname(os.path.abspath(__file__))
PASS_FILE = os.path.expanduser("~/.config/watcher-mqtt/mqtt_pass")


def main():
    print("DEPRECATED: send_cmd.py now routes through send_command.py "
          "(sealed by default). Update your call site to use "
          "send_command.py directly.", file=sys.stderr)

    # Preserve the old password-file convenience.
    if not os.environ.get("MQTT_PASS"):
        try:
            with open(PASS_FILE) as f:
                pw = f.read().strip()
            if pw:
                os.environ["MQTT_PASS"] = pw
        except OSError:
            pass  # send_command.py will report MQTT_PASS is not set

    args = sys.argv[1:]
    # Validate: exactly one JSON command arg plus optional seal flags.
    flags = [a for a in args if a in ("--seal", "--no-seal")]
    rest = [a for a in args if a not in ("--seal", "--no-seal")]
    if len(rest) != 1 or len(flags) > 1:
        print("Usage: send_cmd.py [--seal|--no-seal] '<json command>'",
              file=sys.stderr)
        print("  --seal is the default. --no-seal is ONLY for "
              "seal-rejection testing.", file=sys.stderr)
        sys.exit(2)

    target = os.path.join(BRIDGE_DIR, "send_command.py")
    os.execv(sys.executable, [sys.executable, target] + flags + rest)


if __name__ == "__main__":
    main()
