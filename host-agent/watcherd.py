#!/usr/bin/env python3
"""watcherd — Phase 0 host agent for the Watcher AI input assistant.

The Watcher firmware exposes three host interfaces:
  1. BLE HID keyboard/mouse — the OS pairs directly; no agent needed.
  2. JSON line protocol over USB serial (115200 8N1) — commands/events.
  3. "snap": the device HTTP POSTs a JPEG still to this host.

This CLI talks to (2) and receives (3). It is the "brain extension":
visual decisions, OS-level input fallback, and the bridge a higher-level
AI (e.g. Muse) drives. Cross-platform: macOS and Ubuntu Linux.

Subcommands:
  console       interactive REPL over the serial JSON protocol
  still-server  receive JPEG stills POSTed by the device
  click_at      host-side absolute click at normalized coords (automation)
  type          one-shot: send a "type" command over serial
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from protocol import ProtocolError, decode_line  # noqa: E402
from still_server import StillServer  # noqa: E402
from transport_serial import SerialLink, SerialLinkError  # noqa: E402

CONSOLE_HELP = """\
Commands (shorthand or raw JSON):
  ping                      liveness check -> device replies {"t":"pong"}
  type <text>               type text via BLE HID, e.g.  type hello world
  key <code> [mod]          press key, e.g.  key enter   /  key c ctrl
  mmove <dx> <dy>           relative mouse move, e.g.  mmove 10 -5
  mclick [button]           mouse click (left|right|middle), default left
  wheel <delta>             scroll, e.g.  wheel 1  (negative scrolls down)
  consumer <code>           vol_up|vol_down|mute|play_pause|next|prev
  wifi <ssid> <pass>        store Wi-Fi credentials on the device
  snap <host> [port]        capture JPEG and POST to http://host:port/still?id=N
  { ... }                   raw JSON line, e.g. {"t":"ping"}
  help                      show this text
  quit                      exit (Ctrl-D / Ctrl-C also work)
Device events print as lines starting with '<'.
"""


# ---------------------------------------------------------------------------
# console
# ---------------------------------------------------------------------------

def parse_shorthand(line: str):
    """Parse a REPL line into a protocol command dict.

    Returns the dict, or None when the line was invalid (an error message
    has already been printed). 'quit'/'help' are handled by the caller.
    """
    stripped = line.strip()
    if not stripped:
        return None
    if stripped.startswith("{"):
        try:
            obj = json.loads(stripped)
        except json.JSONDecodeError as exc:
            print(f"error: not valid JSON: {exc}")
            return None
        if not isinstance(obj, dict) or "t" not in obj:
            print("error: raw JSON must be an object with a 't' field")
            return None
        return obj

    parts = stripped.split()
    cmd, args = parts[0].lower(), parts[1:]

    def need(n: int, usage: str) -> bool:
        if len(args) < n:
            print(f"error: usage: {usage}")
            return False
        return True

    def as_int(value: str, what: str):
        try:
            return int(value)
        except ValueError:
            print(f"error: {what} must be an integer, got {value!r}")
            return None

    if cmd == "ping" and not args:
        return {"t": "ping"}
    if cmd == "type":
        text = stripped[len("type"):].lstrip("\t ")
        if not text:
            print("error: usage: type <text>")
            return None
        return {"t": "type", "text": text}
    if cmd == "key":
        if not need(1, "key <code> [mod]"):
            return None
        obj = {"t": "key", "code": args[0]}
        if len(args) > 1:
            obj["mod"] = args[1]
        return obj
    if cmd == "mmove":
        if not need(2, "mmove <dx> <dy>"):
            return None
        dx, dy = as_int(args[0], "dx"), as_int(args[1], "dy")
        if dx is None or dy is None:
            return None
        return {"t": "mouse_move", "dx": dx, "dy": dy}
    if cmd == "mclick":
        obj = {"t": "mouse_click"}
        if args:
            obj["button"] = args[0]
        return obj
    if cmd == "wheel":
        if not need(1, "wheel <delta>"):
            return None
        delta = as_int(args[0], "delta")
        if delta is None:
            return None
        return {"t": "mouse_wheel", "delta": delta}
    if cmd == "consumer":
        if not need(1, "consumer <code>"):
            return None
        return {"t": "consumer", "code": args[0]}
    if cmd == "wifi":
        if not need(2, "wifi <ssid> <pass>"):
            return None
        return {"t": "wifi", "ssid": args[0], "pass": args[1]}
    if cmd == "snap":
        if not need(1, "snap <host> [port]"):
            return None
        obj = {"t": "snap", "host": args[0]}
        if len(args) > 1:
            port = as_int(args[1], "port")
            if port is None:
                return None
            obj["port"] = port
        return obj
    print(f"error: unknown command {cmd!r} (try 'help')")
    return None


def cmd_console(args) -> int:
    link = SerialLink(args.port, baudrate=args.baud)
    try:
        link.connect()
    except SerialLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"Connected to {args.port} @ {args.baud} baud. Type 'help', or 'quit' to exit.")

    def on_event(evt: dict) -> None:
        print(f"< {json.dumps(evt, ensure_ascii=False)}", flush=True)

    def on_error(msg: str) -> None:
        print(f"! {msg}", file=sys.stderr, flush=True)

    link.start_reader(on_event, on_error)
    try:
        while True:
            try:
                line = input("> ")
            except EOFError:
                print()
                break
            if not line.strip():
                continue
            lowered = line.strip().lower()
            if lowered in ("quit", "exit"):
                break
            if lowered in ("help", "?"):
                print(CONSOLE_HELP)
                continue
            obj = parse_shorthand(line)
            if obj is None:
                continue
            try:
                link.send(obj)
            except (SerialLinkError, ProtocolError, ValueError) as exc:
                print(f"error: {exc}")
    except KeyboardInterrupt:
        print()
    finally:
        link.close()
    print("Disconnected.")
    return 0


# ---------------------------------------------------------------------------
# still-server
# ---------------------------------------------------------------------------

def cmd_still_server(args) -> int:
    def on_still(path: str, still_id: int) -> None:
        print(f"still id={still_id} -> {path}", flush=True)

    server = StillServer(host=args.host, port=args.port, out_dir=args.out, on_still=on_still)
    port = server.start()
    print(
        f"Listening for stills at http://{args.host}:{port}/still?id=N\n"
        f"Saving to {os.path.abspath(args.out)}  |  GET /health for liveness\n"
        "Ctrl-C to stop."
    )
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping still server...")
    finally:
        server.stop()
    return 0


# ---------------------------------------------------------------------------
# click_at
# ---------------------------------------------------------------------------

def cmd_click_at(args) -> int:
    if not (0.0 <= args.x <= 1.0 and 0.0 <= args.y <= 1.0):
        print("error: --x and --y must be in [0, 1]", file=sys.stderr)
        return 1
    try:
        import inject
    except ImportError as exc:
        print(f"error: cannot import inject module: {exc}", file=sys.stderr)
        return 1
    try:
        inject.click_at(args.x, args.y)
    except (RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"clicked at normalized ({args.x:.3f}, {args.y:.3f})")
    return 0


# ---------------------------------------------------------------------------
# type (one-shot)
# ---------------------------------------------------------------------------

def cmd_type(args) -> int:
    link = SerialLink(args.port, baudrate=args.baud)
    try:
        link.connect()
    except SerialLinkError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    acks: "queue.Queue[dict]" = queue.Queue()

    def on_event(evt: dict) -> None:
        # Match command acknowledgements for this one-shot send.
        if evt.get("t") in ("ok", "err") and evt.get("cmd") == "type":
            acks.put(evt)

    link.start_reader(on_event)
    try:
        link.send({"t": "type", "text": args.text})
    except (SerialLinkError, ProtocolError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        link.close()
        return 1

    try:
        evt = acks.get(timeout=args.timeout)
    except queue.Empty:
        # The Phase 0 spec only requires acks for some commands; the text may
        # still have been sent. Warn instead of failing hard.
        print(
            "warning: no ok/err ack from device within "
            f"{args.timeout}s (the text may still have been typed)",
            file=sys.stderr,
        )
        link.close()
        return 2
    link.close()
    if evt.get("t") == "err":
        print(f"device error: {evt.get('msg', '')}", file=sys.stderr)
        return 1
    print("ok")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="watcherd",
        description="Phase 0 host agent for the Watcher AI input assistant.",
    )
    sub = parser.add_subparsers(dest="subcommand", required=True)

    p_console = sub.add_parser("console", help="Interactive REPL over the serial JSON protocol.")
    p_console.add_argument("--port", required=True, help="Serial port, e.g. /dev/ttyUSB0")
    p_console.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    p_console.set_defaults(func=cmd_console)

    p_still = sub.add_parser("still-server", help="Receive JPEG stills POSTed by the device.")
    p_still.add_argument("--host", default="0.0.0.0", help="Bind address (default 0.0.0.0)")
    p_still.add_argument("--port", type=int, default=8099, help="Bind port (default 8099)")
    p_still.add_argument("--out", default="./stills", help="Directory for received JPEGs")
    p_still.set_defaults(func=cmd_still_server)

    p_click = sub.add_parser("click_at", help="Host-side absolute click at normalized coords.")
    p_click.add_argument("--x", type=float, required=True, help="Normalized x in [0, 1]")
    p_click.add_argument("--y", type=float, required=True, help="Normalized y in [0, 1]")
    p_click.set_defaults(func=cmd_click_at)

    p_type = sub.add_parser("type", help="One-shot: send a type command over serial.")
    p_type.add_argument("--text", required=True, help="Text for the device to type via BLE HID")
    p_type.add_argument("--port", required=True, help="Serial port, e.g. /dev/ttyUSB0")
    p_type.add_argument("--baud", type=int, default=115200)
    p_type.add_argument("--timeout", type=float, default=3.0, help="Seconds to wait for device ack")
    p_type.set_defaults(func=cmd_type)

    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
