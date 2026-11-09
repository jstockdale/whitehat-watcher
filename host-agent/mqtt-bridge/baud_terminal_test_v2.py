#!/usr/bin/env python3
"""Scripted terminal baud test for the Watcher S3 console UART.

Purpose (John, 2026-11-03): isolate whether 921600 baud is sound on the
device + bridge + wire, independent of the browser / WebSerial layer.
Manual picocom cannot re-open fast enough to catch the device's
confirm window, so this script does the whole switch atomically:

  1. Probe the current rate (ping at each ladder rung until a clean
     pong comes back) -- no state changes, pings only.
  2. Send {"t":"baud","rate":TARGET} at the current rate and wait for
     the baud_ack AT THE OLD RATE (the ack is the timing-critical bit:
     on 0.4.25 its drain wait was a no-op; 0.4.26 guarantees it).
  3. Re-baud the local port in place (termios, no close/open -- the
     WebSerial close/open stall is exactly what we are bypassing),
     send a confirm ping well inside the 8 s auto-revert window, and
     on a parsed pong send {"t":"baud_ok"}: since firmware 0.4.30 the
     revert watchdog disarms ONLY on baud_ok (round-trip proof), so a
     pong alone no longer confirms -- without baud_ok the device
     reverts mid-soak to its last confirmed rate and the soak verdict
     would falsely indict the wire.
  4. Soak: ping repeatedly for --soak seconds, counting clean pongs,
     timeouts and garbled/unparseable lines.
  5. Always try to switch the device back to --restore (default
     115200, the universal boot rate the console re-negotiates from).
     If that fails, wait out the device's own 8 s auto-revert (which
     lands on its last baud_ok-confirmed rate) and probe the ladder
     to find where it settled -- the link can never be stranded.

v2 diagnostic additions (2026-11-04, after the first real run returned
NO-DEVICE on /dev/ttyACM1 while the console link was working fine):
  * Raw-evidence logging: every probe window reports bytes received,
    complete lines seen, text (ESP_LOG/boot) lines and mangled-JSON
    lines, plus the first few raw lines verbatim. v1 counted none of
    this, so total silence, boot chatter on the right channel, and
    garbage on the wrong rate all collapsed into the same NO-DEVICE
    verdict -- the discriminator the run needed was discarded.
  * Settle-and-drain after open: opening a tty can toggle modem lines
    wired to the S3's reset/boot straps. v2 waits --settle seconds
    (default 2.5) after open, draining and logging whatever arrives
    (a boot log in this window PROVES the right channel and a reset
    happened), before the first probe.
  * Multi-round probing: v1 probed each rate exactly once, so a
    reset-on-open boot (device silent at 115200 for the first second
    or two) could race the single pass and yield a false NO-DEVICE.
    v2 probes up to --probe-rounds rounds (default 3) before verdict.

Stdlib only (termios) -- no pyserial needed on the bench Tab.
Run it with the Watcher Console DISCONNECTED (the port must be free).

Exit codes: 0 = locked + soak clean; 2 = switch/confirm failed;
3 = locked but soak saw errors; 4 = could not find the device at any
rate; 5 = restore/revert verification failed.
"""

import argparse
import json
import os
import select
import sys
import termios
import time

LADDER = (115200, 230400, 460800, 921600)
BAUD_CONST = {
    115200: termios.B115200,
    230400: termios.B230400,
    460800: termios.B460800,
    921600: termios.B921600,
}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.buf = b""
        self.garbled = 0
        # v2 raw-evidence counters (cumulative; snapshot per window)
        self.bytes_in = 0
        self.lines_seen = 0
        self.text_lines = 0
        self.samples = []  # first raw lines seen, for the report

    def stats(self):
        return (self.bytes_in, self.lines_seen, self.text_lines,
                self.garbled)

    def stats_delta(self, before):
        b, l, t, g = self.stats()
        return {"bytes": b - before[0], "lines": l - before[1],
                "text_lines": t - before[2], "garbled": g - before[3]}

    def drain(self, duration_s):
        """Collect whatever arrives for duration_s without sending
        anything; used for the post-open settle window. Returns the
        stats delta. Complete lines are recorded as samples."""
        before = self.stats()
        deadline = time.time() + duration_s
        while time.time() < deadline:
            remain = deadline - time.time()
            r, _, _ = select.select([self.fd], [], [], min(remain, 0.25))
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                chunk = b""
            if chunk:
                self.bytes_in += len(chunk)
                self.buf += chunk
                while b"\n" in self.buf:
                    raw, self.buf = self.buf.split(b"\n", 1)
                    raw = raw.strip(b"\r ")
                    if not raw:
                        continue
                    self.lines_seen += 1
                    self.text_lines += 1
                    if len(self.samples) < 8:
                        self.samples.append(raw[:120])
        return self.stats_delta(before)

    def set_baud(self, rate):
        attrs = termios.tcgetattr(self.fd)
        # raw-ish 8N1, no flow control, read enabled
        attrs[0] = 0  # iflag: no IXON/IXOFF/ICRNL translations
        attrs[1] = 0  # oflag: no OPOST translation (keep \n as-is)
        attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0  # lflag: no echo / canonical mode
        attrs[4] = BAUD_CONST[rate]  # ispeed
        attrs[5] = BAUD_CONST[rate]  # ospeed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buf = b""

    def send_line(self, obj):
        os.write(self.fd, (json.dumps(obj) + "\n").encode())

    def read_json(self, timeout_s, want_t=None):
        """Return the next parseable JSON line (optionally filtered by
        its 't' field), or None on timeout. Non-JSON lines (ESP_LOG
        output shares this UART) are skipped; undecodable byte runs
        count as garbled."""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                raw, self.buf = self.buf[:nl], self.buf[nl + 1:]
                raw = raw.strip(b"\r ")
                if not raw:
                    continue
                self.lines_seen += 1
                try:
                    obj = json.loads(raw.decode("utf-8", errors="strict"))
                except (ValueError, UnicodeDecodeError):
                    # ESP_LOG lines are plain text and expected; only
                    # count lines that look like mangled JSON.
                    if raw.startswith(b"{"):
                        self.garbled += 1
                    else:
                        self.text_lines += 1
                        if len(self.samples) < 8:
                            self.samples.append(raw[:120])
                    continue
                if want_t is None or obj.get("t") == want_t:
                    return obj
                continue
            remain = deadline - time.time()
            if remain <= 0:
                break
            r, _, _ = select.select([self.fd], [], [], min(remain, 0.25))
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except BlockingIOError:
                    chunk = b""
                if chunk:
                    self.bytes_in += len(chunk)
                    self.buf += chunk
        return None

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


def ping(port, timeout=1.5):
    port.send_line({"t": "ping"})
    return port.read_json(timeout, want_t="pong") is not None


def switch_rate(port, current, target, ack_timeout=3.0, confirm_timeout=4.0):
    """Run the design-doc switch protocol. Returns True on a clean
    lock (ack seen at old rate, pong at new rate, and baud_ok sent:
    since 0.4.30 the device disarms its revert watchdog ONLY on
    baud_ok, sent after a parsed pong -- round-trip proof)."""
    port.set_baud(current)
    time.sleep(0.05)
    port.send_line({"t": "baud", "rate": target})
    ack = port.read_json(ack_timeout, want_t="baud_ack")
    if ack is None:
        log(f"  no baud_ack at {current} within {ack_timeout}s")
        return False
    log(f"  baud_ack at {current}: {ack}")
    # Switch immediately -- the device is already listening at the new
    # rate and its revert watchdog runs for 8 s from ITS switch.
    port.set_baud(target)
    t0 = time.time()
    if ping(port, timeout=confirm_timeout):
        # Round-trip proven (we parsed the pong); only now may the
        # device be told to stay. baud_ok is consumed by the device
        # and never answered, so this is fire-and-forget.
        port.send_line({"t": "baud_ok"})
        log(f"  pong at {target} after {time.time() - t0:.2f}s -- "
            "LOCKED (baud_ok sent)")
        return True
    log(f"  no pong at {target} within {confirm_timeout}s")
    return False


def probe_rates(port, rounds=3):
    """Ping each ladder rung; return the rate the device answers at,
    or None. Pings only -- no state changes (an unconfirmed rate is
    never baud_ok'd, so probing cannot move the device).

    v2: up to `rounds` full passes (a reset-on-open boot can silence
    the first pass), and every rung logs its raw-evidence delta so a
    NO-DEVICE verdict says WHY: 0 bytes = silence (wrong channel,
    wedged port, or device held in reset/boot); text lines but no
    pong = right channel, device alive but not answering pings;
    garbled '{' lines = bytes at a mismatched rate."""
    for rnd in range(1, rounds + 1):
        for rate in (460800, 115200, 230400, 921600):
            port.set_baud(rate)
            time.sleep(0.05)
            before = port.stats()
            if ping(port, timeout=1.2):
                return rate
            d = port.stats_delta(before)
            log(f"  probe round {rnd} @{rate}: {d['bytes']} bytes, "
                f"{d['lines']} lines ({d['text_lines']} text, "
                f"{d['garbled']} mangled-JSON)")
    return None


def report_samples(port):
    if port.samples:
        log("first raw lines received (verbatim, truncated):")
        for smp in port.samples:
            log(f"    {smp!r}")
    else:
        log("no complete lines were received at any point")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="/dev/ttyUSB0",
                    help="S3 channel of the dual bridge (default /dev/ttyUSB0)")
    ap.add_argument("--from-rate", type=int, choices=LADDER, default=None,
                    help="rate the device is at now (default: probe for it)")
    ap.add_argument("--target", type=int, choices=LADDER, default=921600)
    ap.add_argument("--restore", type=int, choices=LADDER, default=115200,
                    help="rate to leave the device at afterwards")
    ap.add_argument("--soak", type=float, default=20.0,
                    help="seconds of ping soak at the target rate")
    ap.add_argument("--ping-interval", type=float, default=0.5)
    ap.add_argument("--settle", type=float, default=2.5,
                    help="seconds to drain/log after opening the port "
                         "before probing (catches a reset-on-open "
                         "boot log; default 2.5)")
    ap.add_argument("--probe-rounds", type=int, default=3,
                    help="full ladder passes before NO-DEVICE "
                         "(default 3)")
    args = ap.parse_args()

    port = Port(args.port)
    locked = False
    try:
        # 0 (v2). Settle: opening the tty may have reset the S3 via
        # the modem lines. Drain at the boot rate and log whatever
        # arrives -- boot chatter here proves the channel is right.
        if args.settle > 0:
            port.set_baud(115200)
            d = port.drain(args.settle)
            log(f"settle {args.settle:.1f}s @115200 after open: "
                f"{d['bytes']} bytes, {d['lines']} lines")
            report_samples(port)
        # 1. Find the device.
        current = args.from_rate
        if current is None:
            log("probing for the device's current rate (pings only)...")
            current = probe_rates(port, rounds=args.probe_rounds)
            if current is not None:
                log(f"  device answers cleanly at {current}")
            if current is None:
                report_samples(port)
                if port.bytes_in == 0:
                    log("VERDICT: NO-DEVICE / TOTAL-SILENCE -- zero "
                        "bytes arrived in the settle window and every "
                        "probe round. That is a wrong channel, a port "
                        "held by another process, a wedged tty, or a "
                        "device held in reset/download mode -- it is "
                        "NOT a baud-rate problem.")
                elif port.text_lines > 0 and port.garbled == 0:
                    log("VERDICT: NO-DEVICE / ALIVE-BUT-SILENT -- "
                        "readable text (log/boot) lines arrived, so "
                        "the channel and a rate are right, but no "
                        "pong: the app may be booting, in download "
                        "mode, or its link task is not answering.")
                else:
                    log("VERDICT: NO-DEVICE / BYTES-NO-PONG -- bytes "
                        "arrived but no clean pong at any rate; see "
                        "the per-rate counts and samples above.")
                return 4
        else:
            port.set_baud(current)
            if not ping(port):
                log(f"VERDICT: NO-DEVICE at stated --from-rate {current}")
                return 4
            log(f"device confirmed at {current}")

        # 2+3. Switch and confirm.
        log(f"switching {current} -> {args.target} ...")
        locked = switch_rate(port, current, args.target)
        if not locked:
            # An unconfirmed switch reverts at +8 s to the device's
            # last baud_ok-confirmed rate -- usually `current`, NOT
            # args.restore -- so probe the ladder instead of assuming.
            log("switch failed; waiting 9 s for the device auto-revert, "
                "then probing for where it settled...")
            time.sleep(9.0)
            found = probe_rates(port)
            if found is not None:
                log(f"device is back at {found} (auto-revert works)")
                log("VERDICT: SWITCH-FAILED -- wire/device did not hold "
                    f"{args.target} from a terminal either, OR the ack "
                    "never made it out at the old rate (firmware <= "
                    "0.4.25 has the dead drain-wait; 0.4.26 fixes it).")
            else:
                log("VERDICT: SWITCH-FAILED and no answer at any rate "
                    "after the revert window -- check the device is "
                    "powered/awake.")
            return 2

        # 4. Soak.
        sent = got = 0
        garbled_before = port.garbled
        log(f"soaking at {args.target} for {args.soak:.0f}s "
            f"(ping every {args.ping_interval}s)...")
        end = time.time() + args.soak
        while time.time() < end:
            sent += 1
            if ping(port, timeout=max(1.0, args.ping_interval)):
                got += 1
            time.sleep(args.ping_interval)
        garbled = port.garbled - garbled_before
        log(f"soak: {got}/{sent} pongs, {garbled} garbled JSON lines")
        soak_clean = (got == sent and garbled == 0)

        # 5. Restore.
        log(f"restoring {args.target} -> {args.restore} ...")
        if switch_rate(port, args.target, args.restore):
            log(f"device left clean at {args.restore}")
        else:
            # The restore switch may have landed without our seeing
            # the pong (device waiting at restore, watchdog armed):
            # check there FIRST, inside the 8 s window, and baud_ok
            # it if it answers. Only if it is not there do we wait
            # out the revert -- which lands on the last confirmed
            # rate, i.e. the target we just locked, not restore.
            port.set_baud(args.restore)
            if ping(port, timeout=2.0):
                port.send_line({"t": "baud_ok"})
                log(f"device was waiting at {args.restore} -- "
                    "baud_ok sent, left clean there")
            else:
                time.sleep(9.0)
                found = probe_rates(port)
                if found == args.target and switch_rate(
                        port, args.target, args.restore):
                    log(f"device reverted to {args.target}, restore "
                        f"retry left it clean at {args.restore}")
                elif found == args.restore:
                    log(f"device settled at {args.restore} via "
                        "auto-revert")
                else:
                    log("VERDICT: RESTORE-FAILED -- device did not "
                        f"answer at {args.restore} even after the "
                        "revert window. Power-cycle returns it to "
                        "115200 (boot rate).")
                    return 5

        if soak_clean:
            log(f"VERDICT: WIRE-OK -- {args.target} locked from a raw "
                "terminal and soaked clean. If the console still wedges "
                "at this rate, the fault is in the browser/WebSerial "
                "reopen, not the wire, bridge or device.")
            return 0
        log(f"VERDICT: WIRE-MARGINAL -- {args.target} locked but the "
            "soak saw losses/garbling from a raw terminal too. That "
            "indicts the wire/bridge/device side, not WebSerial; "
            "capping Auto at 460800 is the honest call.")
        return 3
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
