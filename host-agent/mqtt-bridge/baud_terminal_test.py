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

Stdlib only (termios) -- no pyserial needed on the bench Tab.
Run it with the Watcher Console DISCONNECTED (the port must be free).

Exit codes: 0 = locked + soak clean; 2 = switch/confirm failed;
3 = locked but soak saw errors; 4 = could not find the device at any
rate; 5 = restore/revert verification failed.

v2 (2026-11-04): every probe step now classifies what it heard --
SILENCE (0 bytes) vs TEXT-ONLY (plain-text lines: ROM/ESP_LOG or the
wrong channel talking) vs GARBAGE (bytes that parse at no rate) vs
JSON -- so a NO-DEVICE verdict says WHY nothing answered. New
--reboot: last-resort EN pulse via the modem lines (RTS -> EN, DTR
held low so IO0 stays high = boot from flash; the console's proven
pulseENtoApp sequence) that returns a wedged S3 to its app at the
115200 boot rate. --from-rate falls back to a full probe when the
stated rate does not answer.

v3 (2026-11-04): the switch confirm phase retries pings across its
window (the console does; a single ping at a marginal rate reads as
total failure when that one packet scrambles). Parsed JSON that is
not the awaited type is now logged verbatim ("heard other JSON") --
v2 counted a mystery JSON line at 115200 without recording what it
was, then misreported "no answer at any rate". switch_rate reports
its own traffic deltas, and verdicts distinguish "no pongs" from
"no device".

v4 (2026-11-04): probe hygiene. A wrong-rate probe ping reaches the
device as a scrambled PARTIAL line (no terminator); the device holds
it in its line buffer, so the next rate's clean ping appends to the
poison and dies as one unparseable line (the device answered
{"t":"err","msg":"unparseable line"} and never ponged, at the very
rate where the probe had just proven it lives). Each probe now sends
a sacrificial lone newline first to terminate any held partial line,
drains the resulting err, and retries its pings. Fallback probes
take a first= rate -- the rate we moved FROM -- because a failed
switch reverts the device to its last confirmed rate: look there
first, before spraying wrong-rate pings anywhere else.

v5 (2026-11-04): --sweep. The CH9102 datasheet allows <2% baud
error on receive and <1.5% on transmit, and the S3 generates
921600 as 922191 (+0.064%): the pair can sit near the edge of the
envelope at 921600 while 460800 stays clean. The sweep varies the
HOST-requested rate around the nominal target (termios2/BOTHER --
cdc-acm forwards the exact rate and the bridge re-picks its
divider, whose quantization error is non-monotonic) while the
device always programs its nominal rate, and measures confirm +
soak quality per point. A host-side sweet spot is directly
shippable: WebSerial accepts arbitrary baudRate values too.

v12 (2026-11-04): --watch-revert. John confirmed the device
REBOOTS during sweep cycles (full boot lightshow, boot
completes well inside the 9 s wait, so sweeps never noticed:
a reboot and a revert both end with the device answering at
115200). Earlier signature watching was deaf because during
the waits the host tty still sat at the foreign rate -- a
115200 boot banner then is garbage bytes. --watch-revert
switches the rate, does NOT follow it up, sends no traffic,
and records every line verbatim at the base rate through the
revert window: the ROM banner's rst: line names the reset
cause (POWERON / SW / WDT / BROWNOUT), and a traffic-free
cycle separates the switch/revert path from the flood path.

v11 (2026-11-04): --trim-sweep points also keep verbatim
corrupt-pong samples (raw + hex) when a point is not clean --
a ratio alone can't say whether residual garble at a plateau
edge is the same phase-slip signature shrinking or a different
failure, and the hex answers it.

v10 (2026-11-04): --trim-sweep. The forensics named the
mechanism -- the bridge receiver loses bit-phase coherence
across a burst at 921600 -- so the trim moves to the side with
an exact divider: the S3. Firmware 0.4.33's baud command takes
"frag" (clkdiv nudge in 1/16 steps) and answers baud_ready with
the achieved rate. The host stays at stock 921600 throughout;
each frag point is a pong-ratio window with no baud_ok, so bad
trims self-revert.

v9 (2026-11-04): --forensics. Counting garble was never going
to name the mechanism; the corruption's SHAPE does. The pong is
a perfect template (exactly {"t":"pong"}), so at a control rate
(460800) and the target, the tool floods pings, captures RAW
lines, aligns every pong-like line against the template and
classifies each bad byte: single-bit flip (sampling at a
transition), bit duplication/deletion (the sampler double- or
under-clocking a bit -- the 0x55->0xD5 signature reported on
this chip), substitution, drop, insert. Position and XOR-mask
histograms plus hex samples make the mechanism legible, and
each class maps to a different UART-usage fix.

v8 (2026-11-04): --scan. The fine sweep closed the trim
question (garble at every host offset; the impairment is edge
quality, not frequency offset). The scan looks for the highest
CLEAN pair rate between proven 460800 and unreachable 921600:
candidates 576000/691200/750000/806400/921600, device switches to
each itself (needs firmware 0.4.32's extended allowlist), host
matches via termios2, same pong-ratio window protocol.

v7 (2026-11-04): --sweep-fine. The coarse sweep's binary
confirm/no-confirm per point proved too noisy at 1% steps (the
link is marginal enough that one point's confirm is a coin flip).
The fine sweep maps pong RATIO instead: 0.25% steps, and per point
a fixed 6.5 s ping window with NO baud_ok ever sent -- quality is
measured, not latched, and every point self-reverts uniformly.
Nominal is re-measured at the end as a drift control.

v6.1 (2026-11-04): reboot signatures (rst: / ESP-ROM / app
banner) are logged the moment they appear ANYWHERE in the run,
not just at startup -- so a reboot near the auto-revert waits or
probes is timestamped evidence, not an impression. (The revert
itself is not a reboot: it is uart_set_baudrate in the running
monitor task, announced as 'baud: no valid traffic -- reverted'.)

v6 (2026-11-04): opening a ttyACM port glitches the modem lines
before userspace can intervene, and on this board RTS is EN: a
glitch can pulse a reboot, or in the worst case hold the S3 in
reset (the silent wedge --reboot rescued). Port now drives
DTR/RTS benign (both false) the instant it opens, and main watches
the first traffic for a boot signature so an open-induced reboot
is logged as an observed fact instead of silently falsifying
rate-state assumptions.
"""

import argparse
import difflib
import fcntl
import json
import os
import select
import struct
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

# Modem-line ioctls (Linux): the CH9102 passes these through to the
# S3's auto-reset circuit -- RTS drives EN, DTR drives IO0.
_TIOCMGET = 0x5415
_TIOCMSET = 0x5418
_TIOCM_DTR = 0x0002
_TIOCM_RTS = 0x0004

# termios2 (Linux) for arbitrary baud rates via BOTHER.
_TCGETS2 = 0x802C542A
_TCSETS2 = 0x402C542B
_BOTHER = 0x1000
_CBAUD = 0x100F


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.buf = b""
        self.garbled = 0
        self.bytes_in = 0
        self.lines_json = 0
        self.lines_text = 0
        self.probe_deltas = []
        self.seen = []  # reprs of parsed JSON that was not awaited
        self.last_text = []  # recent plain-text lines (boot watch)
        self.capture = None  # when a list, read_json keeps raw lines
        # The open itself may have glitched the modem lines (RTS is
        # EN on this board). Drive them benign NOW: releases a held
        # reset, and leaves IO0 high (boot from flash) whatever the
        # open did.
        self.set_signals(False, False)

    def _pre_flush_scan(self):
        """Bytes about to be discarded by a flush may carry a boot
        banner (a reboot that raced our rate change). Scan whatever
        is immediately available before it is thrown away."""
        tail = getattr(self, "_scan_tail", b"") + self.buf
        try:
            for _ in range(64):
                r, _, _ = select.select([self.fd], [], [], 0)
                if not r:
                    break
                chunk = os.read(self.fd, 65536)
                if not chunk:
                    break
                tail = (tail + chunk)[-256:]
        except (BlockingIOError, OSError):
            pass
        self._scan_tail = tail
        for marker in (b"rst:", b"ESP-ROM", b"watcher input"):
            if marker in tail:
                log(f"  !! reboot signature in flushed bytes "
                    f"(marker {marker.decode(errors='replace')})")
                self._scan_tail = b""
                break

    def set_baud(self, rate, flush=True):
        if flush:
            self._pre_flush_scan()
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
        if flush:
            termios.tcflush(self.fd, termios.TCIOFLUSH)
            self.buf = b""

    def set_baud_custom(self, rate):
        """Arbitrary baud via termios2/BOTHER. cdc-acm forwards the
        exact rate to the bridge (SET_LINE_CODING), which re-picks
        its internal divider -- the sweep lever. Same raw 8N1 config
        as set_baud; raises OSError where termios2 is unavailable."""
        raw = fcntl.ioctl(self.fd, _TCGETS2, b"\x00" * 44)
        _if, _of, _cf, _lf, line, cc, _is, _os = struct.unpack(
            "<IIIIB19sII", raw)
        cc = bytearray(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        cflag = ((_cf & ~_CBAUD) | _BOTHER | termios.CLOCAL
                 | termios.CREAD | termios.CS8)
        fcntl.ioctl(self.fd, _TCSETS2, struct.pack(
            "<IIIIB19sII", 0, 0, cflag, 0, line, bytes(cc),
            rate, rate))
        self._pre_flush_scan()
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.buf = b""

    def collect_lines(self, seconds, ping_every=0.4):
        """Raw capture: ping periodically to elicit pongs, return
        EVERY line received (unfiltered, CR stripped) so corrupted
        bytes survive for analysis."""
        lines = []
        end = time.time() + seconds
        last_ping = 0.0
        while time.time() < end:
            now = time.time()
            if now - last_ping >= ping_every:
                self.send_line({"t": "ping"})
                last_ping = now
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    chunk = os.read(self.fd, 65536)
                except BlockingIOError:
                    chunk = b""
                if chunk:
                    self.bytes_in += len(chunk)
                    self.buf += chunk
                    while b"\n" in self.buf:
                        raw, self.buf = self.buf.split(b"\n", 1)
                        lines.append(raw.rstrip(b"\r"))
        return lines

    def send_line(self, obj):
        os.write(self.fd, (json.dumps(obj) + "\n").encode())

    def write_bytes(self, data):
        os.write(self.fd, data)

    def drain(self, seconds):
        """Read and discard until the deadline, so replies provoked
        by a flush byte (e.g. the device's 'unparseable line' err)
        are consumed -- and counted -- before the real exchange."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.read_json(min(0.1, deadline - time.time()),
                           want_t="__drain__")

    def flush_line(self):
        """Terminate any partial line the device is holding (probe
        hygiene: a wrong-rate ping arrives as unterminated garbage
        that would otherwise poison the next clean command line)."""
        self.write_bytes(b"\n")
        self.drain(0.25)

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
                if self.capture is not None \
                        and len(self.capture) < 600:
                    self.capture.append(raw)
                try:
                    obj = json.loads(raw.decode("utf-8", errors="strict"))
                except (ValueError, UnicodeDecodeError):
                    # ESP_LOG lines are plain text and expected; only
                    # count lines that look like mangled JSON.
                    if raw.startswith(b"{"):
                        self.garbled += 1
                    else:
                        self.lines_text += 1
                        text = raw.decode("utf-8", "replace")[:100]
                        self.last_text.append(text)
                        if len(self.last_text) > 6:
                            self.last_text.pop(0)
                        if any(k in text for k in
                               ("rst:", "ESP-ROM", "watcher input")):
                            log(f"  !! reboot signature: {text[:80]}")
                    continue
                self.lines_json += 1
                if want_t is None or obj.get("t") == want_t:
                    return obj
                self.seen.append(json.dumps(obj)[:120])
                if len(self.seen) > 8:
                    self.seen.pop(0)
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

    # -- traffic accounting ------------------------------------------
    def mark(self):
        return (self.bytes_in, self.lines_json, self.lines_text,
                self.garbled)

    def delta(self, m):
        now = self.mark()
        return tuple(a - b for a, b in zip(now, m))

    def seen_mark(self):
        return len(self.seen)

    def seen_since(self, n):
        return self.seen[n:]

    @staticmethod
    def describe(d):
        """Classify a traffic delta: SILENCE vs TEXT-ONLY vs GARBAGE
        vs JSON is the discriminator a bare timeout hides."""
        b, j, t, g = d
        if b == 0:
            kind = "SILENCE"
        elif j > 0:
            kind = "JSON"
        elif t > 0 and g == 0:
            kind = "TEXT-ONLY"
        else:
            kind = "GARBAGE"
        return (f"{b} B in, {j} JSON line(s), {t} text line(s), "
                f"{g} garbled-JSON line(s) -> {kind}")

    # -- modem lines ---------------------------------------------------
    def set_signals(self, dtr, rts):
        try:
            cur = struct.unpack("I", fcntl.ioctl(
                self.fd, _TIOCMGET, struct.pack("I", 0)))[0]
            new = cur & ~(_TIOCM_DTR | _TIOCM_RTS)
            if dtr:
                new |= _TIOCM_DTR
            if rts:
                new |= _TIOCM_RTS
            fcntl.ioctl(self.fd, _TIOCMSET, struct.pack("I", new))
            return True
        except OSError as e:
            log(f"  set_signals failed ({e}); modem lines unavailable "
                "on this port")
            return False

    def pulse_en_to_app(self):
        """Reboot the S3 into its app. Mirrors the console's proven
        pulseENtoApp: DTR false throughout (IO0 high = boot from
        flash), RTS pulses EN. The app boots at 115200."""
        self.set_signals(False, False)
        time.sleep(0.06)
        self.set_signals(False, True)
        time.sleep(0.15)
        self.set_signals(False, False)
        time.sleep(0.5)


def ping(port, timeout=1.5):
    n = port.seen_mark()
    port.send_line({"t": "ping"})
    ok = port.read_json(timeout, want_t="pong") is not None
    if not ok:
        extra = port.seen_since(n)
        if extra:
            log(f"    (ping heard other JSON: {extra})")
    return ok


def switch_rate(port, current, target, ack_timeout=3.0, confirm_timeout=4.0):
    """Run the design-doc switch protocol. Returns True on a clean
    lock (ack seen at old rate, pong at new rate, and baud_ok sent:
    since 0.4.30 the device disarms its revert watchdog ONLY on
    baud_ok, sent after a parsed pong -- round-trip proof)."""
    port.set_baud(current)
    time.sleep(0.05)
    m = port.mark()
    port.send_line({"t": "baud", "rate": target})
    ack = port.read_json(ack_timeout, want_t="baud_ack")
    log(f"  ack-phase traffic at {current}: "
        f"{Port.describe(port.delta(m))}")
    if ack is None:
        log(f"  no baud_ack at {current} within {ack_timeout}s")
        return False
    log(f"  baud_ack at {current}: {ack}")
    # Switch immediately -- the device is already listening at the new
    # rate and its revert watchdog runs for 8 s from ITS switch.
    port.set_baud(target)
    t0 = time.time()
    m = port.mark()
    # Retry pings across the confirm window (the console does the
    # same): at a marginal rate a single scrambled ping must not read
    # as total failure. The whole loop stays well inside the 8 s
    # revert window.
    while time.time() - t0 < confirm_timeout:
        if ping(port, timeout=0.9):
            # Round-trip proven (we parsed the pong); only now may
            # the device be told to stay. baud_ok is consumed by the
            # device and never answered, so this is fire-and-forget.
            port.send_line({"t": "baud_ok"})
            log(f"  pong at {target} after {time.time() - t0:.2f}s "
                "-- LOCKED (baud_ok sent)")
            log(f"  confirm-phase traffic at {target}: "
                f"{Port.describe(port.delta(m))}")
            return True
    log(f"  no pong at {target} within {confirm_timeout}s")
    log(f"  confirm-phase traffic at {target}: "
        f"{Port.describe(port.delta(m))}")
    return False


def sweep_point(port, base, nominal, host_rate, soak_s):
    """One sweep point: from the solid base rate, switch the device
    to its nominal target while the HOST port runs at host_rate.
    Confirm and a short soak run BEFORE any baud_ok, so a bad point
    costs nothing -- the device auto-reverts to base. A clean point
    earns baud_ok, an extended soak, and a commanded return."""
    row = {"host": host_rate, "confirmed": False, "latency": None,
           "sent": 0, "got": 0, "garbled": 0, "note": ""}
    port.set_baud(base)
    time.sleep(0.05)
    port.flush_line()
    port.send_line({"t": "baud", "rate": nominal})
    if port.read_json(3.0, want_t="baud_ack") is None:
        row["note"] = "no ack at base"
        return row
    try:
        port.set_baud_custom(host_rate)
    except OSError as e:
        row["note"] = f"termios2 failed ({e}); waiting out revert"
        return row
    t0 = time.time()
    while time.time() - t0 < 3.0:
        if ping(port, timeout=0.9):
            row["confirmed"] = True
            row["latency"] = time.time() - t0
            break
    if not row["confirmed"]:
        row["note"] = "no confirm; watchdog reverts"
        return row
    g0 = port.garbled
    for _ in range(4):
        row["sent"] += 1
        if ping(port, timeout=0.9):
            row["got"] += 1
    row["garbled"] = port.garbled - g0
    if row["got"] < row["sent"]:
        row["note"] = "pre-ok soak losses; watchdog left armed"
        return row
    port.send_line({"t": "baud_ok"})
    end = time.time() + soak_s
    while time.time() < end:
        row["sent"] += 1
        if ping(port, timeout=1.0):
            row["got"] += 1
        time.sleep(0.25)
    row["garbled"] = port.garbled - g0
    port.send_line({"t": "baud", "rate": base})
    if port.read_json(3.0, want_t="baud_ack") is None:
        row["note"] = ("RETURN ack missing -- device stays at "
                       f"{nominal}; recover with --reboot or rerun "
                       f"with --from-rate {nominal}")
        return row
    port.set_baud(base)
    if ping(port, timeout=2.0) or ping(port, timeout=2.0):
        port.send_line({"t": "baud_ok"})
        row["note"] = "ok"
    else:
        row["note"] = "return unconfirmed (watchdog settles it)"
    return row


def ensure_at_base(port, base):
    """After a rough point, make sure the device is back at base
    before the next one. Returns True when it answers there."""
    port.set_baud(base)
    time.sleep(0.05)
    port.flush_line()
    if ping(port, timeout=0.8) or ping(port, timeout=0.8):
        return True
    found = probe_rates(port, first=base)
    if found == base:
        return True
    if found is not None and switch_rate(port, found, base):
        return True
    return False


def run_sweep(port, base, nominal, soak_s):
    mults = (1.0, 0.99, 1.01, 0.98, 1.02, 0.97, 1.03)
    rates = []
    for mlt in mults:
        r = int(round(nominal * mlt))
        if r not in rates:
            rates.append(r)
    log(f"sweep: device nominal {nominal}, host rates {rates}, "
        f"base {base}")
    rows = []
    for host_rate in rates:
        if rows and not ensure_at_base(port, base):
            log("sweep aborted: device not back at base "
                f"{base} (recover with --reboot)")
            break
        log(f"point: host {host_rate} "
            f"({(host_rate / nominal - 1) * 100:+.1f}%) ...")
        row = sweep_point(port, base, nominal, host_rate, soak_s)
        rows.append(row)
        log(f"  -> confirmed={row['confirmed']} "
            f"latency={row['latency']} pongs {row['got']}/{row['sent']} "
            f"garbled {row['garbled']} note: {row['note']}")
        if not row["confirmed"] or "watchdog" in row["note"]:
            log("  waiting 9 s for the auto-revert...")
            time.sleep(9.0)
    log("sweep results (host rate vs device nominal "
        f"{nominal}, device actual per its clkdiv):")
    best = None
    for row in rows:
        pct = (row["host"] / nominal - 1) * 100
        score = (row["got"] / row["sent"]) if row["sent"] else 0.0
        log(f"  host {row['host']} ({pct:+.1f}%): "
            f"confirmed={row['confirmed']} pongs "
            f"{row['got']}/{row['sent']} garbled {row['garbled']} "
            f"-- {row['note']}")
        if (row["note"] == "ok" and row["garbled"] == 0
                and (best is None or score > best[0])):
            best = (score, row["host"])
    if best is not None:
        log(f"SWEEP VERDICT: cleanest host rate {best[1]} "
            f"({(best[1] / nominal - 1) * 100:+.1f}% vs nominal). "
            "If that differs from nominal, the pair's sweet spot is "
            "offset -- the console can request the same rate via "
            "WebSerial, no firmware change needed.")
        return 0
    log("SWEEP VERDICT: no fully clean point. Read the curve: a "
        "peak off nominal = sampling-margin (trim helps); flat-bad "
        "everywhere = the rate itself is beyond this link.")
    return 3


def sweep_fine_point(port, base, nominal, host_rate, window_s):
    """One fine point: switch the device to nominal, run the host
    at host_rate, and count pongs over a fixed window. baud_ok is
    NEVER sent -- the point is a measurement, and the device
    auto-reverts to base when the window closes."""
    row = {"host": host_rate, "sent": 0, "got": 0, "garbled": 0,
           "bytes": 0, "note": ""}
    port.set_baud(base)
    time.sleep(0.05)
    port.flush_line()
    port.send_line({"t": "baud", "rate": nominal})
    if port.read_json(3.0, want_t="baud_ack") is None:
        row["note"] = "no ack at base"
        return row
    try:
        port.set_baud_custom(host_rate)
    except OSError as e:
        row["note"] = f"termios2 failed ({e})"
        return row
    m = port.mark()
    deadline = time.time() + window_s
    while time.time() < deadline:
        row["sent"] += 1
        if ping(port, timeout=0.55):
            row["got"] += 1
        time.sleep(0.05)
    d = port.delta(m)
    row["bytes"] = d[0]
    row["garbled"] = d[3]
    return row


def run_sweep_fine(port, base, nominal, lo, hi, step, window_s):
    pcts = []
    p = lo
    while p <= hi + 1e-9:
        pcts.append(round(p, 4))
        p += step
    rates = [int(round(nominal * (1 + pc / 100.0))) for pc in pcts]
    log(f"fine sweep: device nominal {nominal}, base {base}, "
        f"host offsets {pcts}%, window {window_s}s per point")
    rows = []
    for pc, host_rate in list(zip(pcts, rates)) + [(0.0, nominal)]:
        if rows and not ensure_at_base(port, base):
            log("fine sweep aborted: device not back at base "
                f"{base} (recover with --reboot)")
            break
        tag = " (drift control)" if len(rows) == len(rates) else ""
        log(f"point: host {host_rate} ({pc:+.2f}%){tag} ...")
        row = sweep_fine_point(port, base, nominal, host_rate,
                               window_s)
        row["pct"] = pc
        rows.append(row)
        ratio = (row["got"] / row["sent"]) if row["sent"] else 0.0
        log(f"  -> pongs {row['got']}/{row['sent']} ({ratio:.2f}), "
            f"garbled {row['garbled']}, {row['bytes']} B "
            f"{row['note']}")
        log("  waiting 9 s for the auto-revert...")
        time.sleep(9.0)
    log("fine sweep results (pong ratio per host offset; device "
        f"nominal {nominal} throughout):")
    best = None
    for row in rows:
        ratio = (row["got"] / row["sent"]) if row["sent"] else 0.0
        log(f"  host {row['host']} ({row['pct']:+.2f}%): "
            f"{row['got']}/{row['sent']} = {ratio:.2f}, garbled "
            f"{row['garbled']} {row['note']}")
        if row["sent"] >= 4 and (best is None or ratio > best[0]):
            best = (ratio, row["host"], row["pct"])
    if best is not None and best[0] >= 0.9:
        log(f"FINE SWEEP VERDICT: peak at host {best[1]} "
            f"({best[2]:+.2f}%) with pong ratio {best[0]:.2f} -- "
            "validate that rate with the coarse --sweep (it "
            "baud_oks and soaks), then the console can request the "
            "same rate via WebSerial.")
        return 0
    if best is not None:
        log(f"FINE SWEEP VERDICT: best point host {best[1]} "
            f"({best[2]:+.2f}%) only reached {best[0]:.2f} -- no "
            "sweet spot on the host side. The remaining lever is "
            "trimming the S3's own clkdiv (firmware).")
        return 3
    log("FINE SWEEP VERDICT: no measurable point at all.")
    return 3


def run_scan(port, base, window_s):
    rates = (576000, 691200, 750000, 806400, 921600)
    log(f"rate scan: candidates {rates}, base {base}, window "
        f"{window_s}s per point (device switches to each rate; "
        "needs firmware >= 0.4.32)")
    rows = []
    for rate in rates:
        if rows and not ensure_at_base(port, base):
            log("scan aborted: device not back at base "
                f"{base} (recover with --reboot)")
            break
        log(f"point: pair rate {rate} ...")
        row = sweep_fine_point(port, base, rate, rate, window_s)
        row["rate"] = rate
        rows.append(row)
        ratio = (row["got"] / row["sent"]) if row["sent"] else 0.0
        log(f"  -> pongs {row['got']}/{row['sent']} ({ratio:.2f}), "
            f"garbled {row['garbled']}, {row['bytes']} B "
            f"{row['note']}")
        log("  waiting 9 s for the auto-revert...")
        time.sleep(9.0)
    log("rate scan results:")
    best = None
    for row in rows:
        ratio = (row["got"] / row["sent"]) if row["sent"] else 0.0
        clean = ratio >= 0.97 and row["garbled"] == 0
        log(f"  {row['rate']}: {row['got']}/{row['sent']} = "
            f"{ratio:.2f}, garbled {row['garbled']} "
            f"{'CLEAN' if clean else ''} {row['note']}")
        if clean:
            best = row["rate"]
    if best is not None:
        log(f"SCAN VERDICT: highest clean pair rate {best} "
            f"({best / 460800 - 1:+.0%} vs 460800). That becomes "
            "the console ladder's top rung.")
        return 0
    log("SCAN VERDICT: no clean rate above 460800 -- 460800 is "
        "the honest ceiling for this link.")
    return 3


PONG_TMPL = b'{"t":"pong"}'


def _bits(b):
    return [(b >> i) & 1 for i in range(8)]


def _from_bits(bs):
    v = 0
    for i, bit in enumerate(bs[:8]):
        v |= bit << i
    return v


def _dup_bit(b, k):
    bs = _bits(b)
    bs.insert(k, bs[k])
    return _from_bits(bs)


def _del_bit(b, k):
    bs = _bits(b)
    del bs[k]
    return _from_bits(bs)


def classify_byte(exp, got):
    """Classify one corrupted byte against its expected value.
    flip = XOR is a single bit (sampled at a transition);
    dup/del = one bit duplicated or dropped inside the byte (the
    sampler clocked a bit twice or skipped it -- a frame-level
    timing slip, the 0x55->0xD5 signature); subst = anything else."""
    if got == exp:
        return ("ok", 0)
    mask = exp ^ got
    if mask & (mask - 1) == 0:
        return ("flip", mask)
    for k in range(8):
        if _dup_bit(exp, k) == got:
            return ("dup", k)
    for k in range(8):
        if _del_bit(exp, k) == got:
            return ("del", k)
    return ("subst", mask)


def analyze_lines(lines, label):
    stats = {"label": label, "lines": len(lines), "clean_pong": 0,
             "corrupt_pong": 0, "other_json": 0, "text_lines": 0,
             "corrupt_text": 0, "events": [], "pos": {},
             "masks": {}, "kinds": {}, "samples": []}
    for raw in lines:
        if raw == PONG_TMPL:
            stats["clean_pong"] += 1
            continue
        # Pong-likeness BEFORE the JSON bucket: a bit flip inside a
        # letter can yield valid-but-wrong JSON ({"t":"polog"}),
        # which is a corrupt pong, not other traffic.
        pong_like = (raw.startswith(b"{")
                     and (abs(len(raw) - len(PONG_TMPL)) <= 3
                          or b'"pong"' in raw))
        if not pong_like:
            try:
                json.loads(raw.decode("utf-8", errors="strict"))
                stats["other_json"] += 1
                continue
            except (ValueError, UnicodeDecodeError):
                pass
            # Text bucket: clean ASCII text is expected (ESP_LOG);
            # non-printable bytes inside it are corruption too.
            if all(32 <= c < 127 for c in raw):
                stats["text_lines"] += 1
            else:
                stats["corrupt_text"] += 1
                if len(stats["samples"]) < 8:
                    stats["samples"].append(("text", None, raw))
            continue
        stats["corrupt_pong"] += 1
        sm = difflib.SequenceMatcher(None, PONG_TMPL, raw,
                                     autojunk=False)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op == "equal":
                continue
            if op == "delete":
                for _ in range(i2 - i1):
                    stats["kinds"]["drop"] = \
                        stats["kinds"].get("drop", 0) + 1
            elif op == "insert":
                for _ in range(j2 - j1):
                    stats["kinds"]["insert"] = \
                        stats["kinds"].get("insert", 0) + 1
            elif op == "replace":
                for a, b in zip(range(i1, i2), range(j1, j2)):
                    kind, info = classify_byte(PONG_TMPL[a], raw[b])
                    stats["kinds"][kind] = \
                        stats["kinds"].get(kind, 0) + 1
                    stats["pos"][a] = stats["pos"].get(a, 0) + 1
                    if kind in ("flip", "subst"):
                        stats["masks"][info] = \
                            stats["masks"].get(info, 0) + 1
                    if kind == "flip" and any(
                            _dup_bit(PONG_TMPL[a], k) == raw[b]
                            or _del_bit(PONG_TMPL[a], k) == raw[b]
                            for k in range(8)):
                        # e.g. 0x55->0xD5 reads as either an MSB
                        # flip or a duplicated bit; tally the
                        # ambiguity instead of forcing a story.
                        stats["kinds"]["flip~dup_ambig"] = \
                            stats["kinds"].get("flip~dup_ambig", 0) + 1
                    stats["events"].append((a, kind, info))
                extra_e = (i2 - i1) - (j2 - j1)
                if extra_e > 0:
                    stats["kinds"]["drop"] = \
                        stats["kinds"].get("drop", 0) + extra_e
                elif extra_e < 0:
                    stats["kinds"]["insert"] = \
                        stats["kinds"].get("insert", 0) - extra_e
        if len(stats["samples"]) < 8:
            stats["samples"].append(("pong", PONG_TMPL, raw))
    return stats


def report_forensics(stats):
    label = stats["label"]
    log(f"forensics @ {label}: {stats['lines']} lines, "
        f"{stats['clean_pong']} clean pongs, "
        f"{stats['corrupt_pong']} corrupt pongs, "
        f"{stats['other_json']} other JSON, "
        f"{stats['text_lines']} clean text, "
        f"{stats['corrupt_text']} corrupt text")
    if stats["kinds"]:
        kinds = ", ".join(f"{k}={v}" for k, v in
                          sorted(stats["kinds"].items(),
                                 key=lambda kv: -kv[1]))
        log(f"  corruption classes: {kinds}")
    if stats["pos"]:
        pos = ", ".join(f"byte {k}: {v}" for k, v in
                        sorted(stats["pos"].items()))
        log(f"  corrupt positions in the 12-byte pong: {pos}")
    if stats["masks"]:
        top = sorted(stats["masks"].items(),
                     key=lambda kv: -kv[1])[:6]
        log("  top XOR masks: " + ", ".join(
            f"0x{m:02x} x{n}" for m, n in top))
    for kind, exp, got in stats["samples"][:4]:
        if kind == "pong" and exp is not None:
            log(f"  exp {exp.hex(' ')}  {exp.decode(errors='replace')}")
            log(f"  got {got.hex(' ')}  "
                f"{got.decode(errors='replace')}")
        else:
            log(f"  text sample: {got[:64]!r}")


def run_forensics(port, current, target, seconds):
    control = 460800
    home = current
    if current != control:
        log(f"forensics: moving {current} -> {control} (control)...")
        if not switch_rate(port, current, control):
            log("control switch failed; aborting forensics")
            return 2
        current = control
    log(f"forensics: collecting at control {control} for "
        f"{seconds:.0f}s...")
    report_forensics(analyze_lines(
        port.collect_lines(seconds), f"{control} (control)"))
    log(f"forensics: switching {control} -> {target} ...")
    if switch_rate(port, control, target):
        log(f"forensics: collecting at {target} for {seconds:.0f}s...")
        report_forensics(analyze_lines(
            port.collect_lines(seconds), f"{target}"))
        if switch_rate(port, target, home):
            log(f"forensics: device restored to {home}")
        else:
            log(f"forensics: restore to {home} unconfirmed; the "
                "revert watchdog settles it")
    else:
        log(f"forensics: could not confirm at {target} this time; "
            "no target capture (the link's intermittency is itself "
            "the datum). Device reverts to its last confirmed rate.")
    return 0


def expected_actual(rate, frag):
    """The S3's achieved rate for a nominal rate + frag trim.
    IDF's divider pick is truncating (40 MHz << 4) // rate --
    matches the 0.4.27 diag readback (921600 -> total 694 =
    43*16+6 -> 922191). frag adds directly to that total."""
    tot16 = (40_000_000 * 16) // rate + frag
    return int(round(40_000_000 * 16 / tot16))


def sweep_trim_point(port, base, nominal, frag, window_s):
    """One trim point: device switches to nominal+frag, host stays
    at the STOCK nominal rate. baud_ready (when it survives) gives
    the achieved-rate ground truth. No baud_ok: measurement only,
    the watchdog reverts the trim with the rate."""
    row = {"frag": frag, "actual": None,
           "expected": expected_actual(nominal, frag),
           "sent": 0, "got": 0, "garbled": 0, "bytes": 0, "note": "",
           "samples": []}
    port.set_baud(base)
    time.sleep(0.05)
    port.flush_line()
    port.send_line({"t": "baud", "rate": nominal, "frag": frag})
    if port.read_json(3.0, want_t="baud_ack") is None:
        row["note"] = "no ack at base (firmware >= 0.4.33 required)"
        return row
    port.set_baud(nominal)
    ready = port.read_json(0.8, want_t="baud_ready")
    if ready is not None:
        try:
            row["actual"] = int(ready.get("actual", 0)) or None
        except (TypeError, ValueError):
            row["actual"] = None
    m = port.mark()
    port.capture = []
    deadline = time.time() + window_s
    while time.time() < deadline:
        row["sent"] += 1
        if ping(port, timeout=0.55):
            row["got"] += 1
        time.sleep(0.05)
    caps, port.capture = port.capture, None
    for raw in caps or []:
        if raw != b'{"t":"pong"}' and raw.startswith(b'{"t":"po'):
            row["samples"].append(raw)
            if len(row["samples"]) >= 3:
                break
    d = port.delta(m)
    row["bytes"] = d[0]
    row["garbled"] = d[3]
    return row


def run_trim_sweep(port, base, nominal, lo, hi, step, window_s):
    frags = list(range(lo, hi + 1, step))
    if 0 not in frags:
        frags.append(0)
        frags.sort()
    log(f"trim sweep: device nominal {nominal}, frag {frags}, "
        f"host stock {nominal}, base {base} (needs fw >= 0.4.33)")
    rows = []
    for frag in frags:
        if rows and not ensure_at_base(port, base):
            log("trim sweep aborted: device not back at base "
                f"{base} (recover with --reboot)")
            break
        log(f"point: frag {frag:+d} (expected S3 actual "
            f"{expected_actual(nominal, frag)}) ...")
        row = sweep_trim_point(port, base, nominal, frag, window_s)
        rows.append(row)
        ratio = (row["got"] / row["sent"]) if row["sent"] else 0.0
        act = row["actual"] if row["actual"] else row["expected"]
        log(f"  -> pongs {row['got']}/{row['sent']} ({ratio:.2f}), "
            f"garbled {row['garbled']}, S3 actual {act} "
            f"{row['note']}")
        for s in row.get("samples", []):
            log(f"    corrupt sample: {s.hex(' ')}  "
                f"{s.decode('utf-8', 'replace')}")
        log("  waiting 9 s for the auto-revert...")
        time.sleep(9.0)
    log("trim sweep results (host stock "
        f"{nominal} throughout):")
    best = None
    for row in rows:
        ratio = (row["got"] / row["sent"]) if row["sent"] else 0.0
        act = row["actual"] if row["actual"] else row["expected"]
        log(f"  frag {row['frag']:+d} (S3 actual {act}): "
            f"{row['got']}/{row['sent']} = {ratio:.2f}, garbled "
            f"{row['garbled']} {row['note']}")
        if row["sent"] >= 4 and (best is None or ratio > best[0]):
            best = (ratio, row)
    clean = [r for r in rows if r["sent"] >= 4
             and r["got"] / r["sent"] >= 0.97 and r["garbled"] == 0]
    if clean:
        # The middle of the clean plateau has the most margin in
        # both directions (temperature, cable, unit variance).
        mid = clean[len(clean) // 2]
        act = mid["actual"] if mid["actual"] else mid["expected"]
        ratio = mid["got"] / mid["sent"]
        log(f"TRIM VERDICT: frag {mid['frag']:+d} is CLEAN "
            f"(S3 actual {act}, ratio {ratio:.2f}; clean plateau "
            f"{clean[0]['frag']:+d}..{clean[-1]['frag']:+d}, "
            "picking its middle). Bake this frag into the "
            "921600 rung.")
        return 0
    if best is not None and best[0] >= 0.9:
        row = best[1]
        act = row["actual"] if row["actual"] else row["expected"]
        log(f"TRIM VERDICT: frag {row['frag']:+d} is close "
            f"(S3 actual {act}, ratio {best[0]:.2f}) -- refine "
            "around it with --trim-step 1 before baking.")
        return 0
    log("TRIM VERDICT: no frag in range cleans the link. The "
        "rate itself is the ceiling; fall back to the --scan "
        "mid-rates or 460800.")
    return 3


def watch_revert(port, base, target, seconds=18.0):
    """Switch the device to target, then sit at base and record
    EVERYTHING through the revert window. No traffic is sent:
    whatever happens is the switch/revert path alone. Returns 0
    if a clean revert was observed, 4 if a reboot signature
    appeared, 3 otherwise."""
    port.set_baud(base)
    time.sleep(0.05)
    port.flush_line()
    port.capture = []
    port.send_line({"t": "baud", "rate": target})
    ack = port.read_json(3.0, want_t="baud_ack")
    if ack is None:
        port.capture = None
        log("watch: no baud_ack at base -- cannot start")
        return 2
    log(f"watch: acked switch to {target}; host stays at {base}, "
        f"recording for {seconds:.0f} s (revert watchdog is 8 s). "
        "NO traffic will be sent.")
    seen = 0
    booted = False
    deadline = time.time() + seconds
    while time.time() < deadline:
        port.read_json(0.25)
        caps = port.capture or []
        while seen < len(caps):
            raw = caps[seen]
            seen += 1
            text = raw.decode("utf-8", "replace")
            if any(k in text for k in
                   ("rst:", "ESP-ROM", "watcher input", "boot:")):
                booted = True
            log(f"  wire: {text}")
    port.capture = None
    alive = ping(port, timeout=1.5)
    log(f"watch: device answers at {base} afterwards: "
        f"{'yes' if alive else 'NO'}")
    if booted:
        log("WATCH VERDICT: REBOOT observed -- the rst: line "
            "above names the cause (POWERON=EN/power, SW=software"
            " restart, TG/WDT=panic or watchdog, BROWNOUT=rail).")
        return 4
    log("WATCH VERDICT: no boot banner in the window -- the "
        "revert was clean in a traffic-free cycle. If reboots "
        "only happen under sweep traffic, the trigger is the "
        "flood/garbage path, not the switch/revert path.")
    return 0


def probe_rates(port, first=None):
    """Ping each ladder rung; return the rate the device answers at,
    or None. Pings only -- no state changes (an unconfirmed rate is
    never baud_ok'd, so probing cannot move the device). Each rate's
    traffic is classified and logged (SILENCE / TEXT-ONLY / GARBAGE /
    JSON) and the deltas stashed on port.probe_deltas so a NO-DEVICE
    verdict can say what was actually heard. `first` moves one rate
    to the head of the order: fallback probes pass the rate we moved
    from, where an auto-reverted device will be sitting."""
    order = (460800, 115200, 230400, 921600)
    if first in order:
        order = (first,) + tuple(r for r in order if r != first)
    port.probe_deltas = []
    for rate in order:
        port.set_baud(rate)
        time.sleep(0.05)
        m = port.mark()
        port.flush_line()
        ok = False
        for _attempt in range(3):
            if ping(port, timeout=0.8):
                ok = True
                break
        d = port.delta(m)
        port.probe_deltas.append(d)
        log(f"  probe {rate}: {Port.describe(d)}")
        if ok:
            return rate
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", default="/dev/ttyUSB0",
                    help="S3 channel of the dual bridge (default /dev/ttyUSB0)")
    ap.add_argument("--from-rate", type=int, choices=LADDER, default=None,
                    help="rate the device is at now (default: probe for it)")
    ap.add_argument("--reboot", action="store_true",
                    help="last resort: pulse EN via RTS (DTR held low, "
                         "boot from flash) to reboot a wedged S3 back "
                         "to its 115200 boot rate before anything else")
    ap.add_argument("--target", type=int, choices=LADDER, default=921600)
    ap.add_argument("--restore", type=int, choices=LADDER, default=115200,
                    help="rate to leave the device at afterwards")
    ap.add_argument("--sweep", action="store_true",
                    help="sweep the HOST-requested rate around "
                         "--target (device stays nominal) and table "
                         "confirm/soak quality per offset")
    ap.add_argument("--sweep-soak", type=float, default=6.0,
                    help="extended soak seconds per clean sweep point")
    ap.add_argument("--watch-revert", action="store_true",
                    help="switch to --target, stay at the base "
                         "rate, send nothing, and record all wire "
                         "traffic verbatim through the revert "
                         "window (reboot/reset-cause hunt)")
    ap.add_argument("--trim-sweep", action="store_true",
                    help="S3 clkdiv trim sweep: device takes "
                         "--target with frag offsets (fw >= "
                         "0.4.33), host stays at stock --target; "
                         "pong-ratio per frag step")
    ap.add_argument("--trim-lo", type=int, default=-14)
    ap.add_argument("--trim-hi", type=int, default=14)
    ap.add_argument("--trim-step", type=int, default=2)
    ap.add_argument("--trim-window", type=float, default=5.5,
                    help="measurement window per trim point "
                         "(0.8 s baud_ready wait + window must fit "
                         "the 8 s revert window)")
    ap.add_argument("--forensics", action="store_true",
                    help="corruption forensics: raw-line capture at "
                         "460800 (control) and --target, aligning "
                         "pongs against the known template and "
                         "classifying every bad byte")
    ap.add_argument("--forensics-seconds", type=float, default=20.0,
                    help="capture window per rate for --forensics")
    ap.add_argument("--scan", action="store_true",
                    help="rate scan: pong-ratio at each candidate "
                         "pair rate 576000..921600 (needs firmware "
                         ">= 0.4.32); finds the highest clean rate")
    ap.add_argument("--sweep-fine", action="store_true",
                    help="fine sweep: pong-ratio per host offset "
                         "(0.25%% steps), no baud_ok -- a curve, "
                         "not coin flips")
    ap.add_argument("--sweep-fine-lo", type=float, default=-1.0,
                    help="fine sweep lowest offset %% (default -1)")
    ap.add_argument("--sweep-fine-hi", type=float, default=2.5,
                    help="fine sweep highest offset %% (default 2.5)")
    ap.add_argument("--sweep-fine-step", type=float, default=0.25,
                    help="fine sweep offset step %% (default 0.25)")
    ap.add_argument("--sweep-fine-window", type=float, default=6.5,
                    help="measurement window per fine point, seconds "
                         "(must stay inside the 8 s revert window)")
    ap.add_argument("--soak", type=float, default=20.0,
                    help="seconds of ping soak at the target rate")
    ap.add_argument("--ping-interval", type=float, default=0.5)
    args = ap.parse_args()

    port = Port(args.port)
    # Watch the first traffic for a boot signature: if the open
    # glitched EN, the device rebooted and sits at its 115200 boot
    # rate whatever it was doing before. Log it as a fact, not a
    # confound.
    port.set_baud(115200, flush=False)
    port.drain(1.4)
    boot_marks = [t for t in port.last_text
                  if any(k in t for k in ("rst:", "ESP-ROM",
                                          "watcher input"))]
    if boot_marks:
        log("note: boot signature at port open -- the device "
            "rebooted (modem-line glitch) and is at its 115200 "
            f"boot rate: {boot_marks[-1][:80]}")
    locked = False
    try:
        # 1. Find the device.
        current = None
        if args.reboot:
            log("reboot requested: pulsing EN (RTS), DTR held low "
                "(boot from flash)...")
            port.set_baud(115200)
            m = port.mark()
            port.pulse_en_to_app()
            deadline = time.time() + 6.0
            while time.time() < deadline:
                if ping(port, timeout=1.0):
                    current = 115200
                    break
                time.sleep(0.3)
            log(f"  boot traffic: {Port.describe(port.delta(m))}")
            if current is not None:
                log("  device rebooted and answers at 115200 "
                    "(boot rate)")
            else:
                log("VERDICT: NO-DEVICE even after an EN pulse -- "
                    "boot traffic above says why (SILENCE = the "
                    "reset did not reach the S3, wrong port, or a "
                    "wedged tty; TEXT-ONLY = it booted but the app "
                    "is not answering).")
                return 4
        if current is None and args.from_rate is not None:
            port.set_baud(args.from_rate)
            time.sleep(0.05)
            port.flush_line()
            if ping(port, timeout=0.8) or ping(port, timeout=0.8):
                current = args.from_rate
                log(f"device confirmed at {current}")
            else:
                log(f"  no pong at stated --from-rate "
                    f"{args.from_rate}; falling back to a full "
                    "probe...")
        if current is None:
            log("probing for the device's current rate (pings only)...")
            current = probe_rates(port)
            if current is not None:
                log(f"  device answers cleanly at {current}")
            if current is None:
                tot_b = sum(d[0] for d in port.probe_deltas)
                tot_j = sum(d[1] for d in port.probe_deltas)
                tot_t = sum(d[2] for d in port.probe_deltas)
                if tot_b == 0:
                    why = ("SILENCE at every rate -- nothing is "
                           "transmitting: wrong channel, S3 held in "
                           "reset/bootloader, unplugged, or a wedged "
                           "tty. Try --reboot, then a physical replug.")
                elif tot_j > 0:
                    why = ("JSON heard but no pongs -- the device is "
                           "alive and speaking the link protocol but "
                           "did not answer pings; see the 'heard "
                           "other JSON' lines above for what it said.")
                elif tot_t > 0:
                    why = ("TEXT heard but no JSON -- something alive "
                           "speaks plain text here (ROM chatter, "
                           "ESP_LOG with no app link, or the other "
                           "bridge channel), not the app's JSON port.")
                else:
                    why = ("GARBAGE heard -- bytes arrive but parse "
                           "at no ladder rate: a scrambled link, or a "
                           "speaker at a non-ladder rate.")
                log(f"VERDICT: NO-DEVICE -- no clean pong at any "
                    f"rate. {why}")
                return 4

        if args.forensics:
            return run_forensics(port, current, args.target,
                                 args.forensics_seconds)

        if args.watch_revert:
            if current == args.target:
                log("watch: device already at the target rate; "
                    "nothing to switch")
                return 2
            return watch_revert(port, current, args.target)

        if args.sweep or args.sweep_fine or args.scan \
                or args.trim_sweep:
            base = current
            if base == args.target:
                log(f"device already at the sweep target "
                    f"{args.target}; stepping down to 115200 as the "
                    "sweep base...")
                if not switch_rate(port, base, 115200):
                    log("could not step down; aborting sweep")
                    return 2
                base = 115200
            if args.trim_sweep:
                return run_trim_sweep(
                    port, base, args.target, args.trim_lo,
                    args.trim_hi, args.trim_step, args.trim_window)
            if args.scan:
                return run_scan(port, base, args.sweep_fine_window)
            if args.sweep_fine:
                return run_sweep_fine(
                    port, base, args.target, args.sweep_fine_lo,
                    args.sweep_fine_hi, args.sweep_fine_step,
                    args.sweep_fine_window)
            return run_sweep(port, base, args.target, args.sweep_soak)

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
            found = probe_rates(port, first=current)
            if found is not None:
                log(f"device is back at {found} (auto-revert works)")
                log("VERDICT: SWITCH-FAILED -- wire/device did not hold "
                    f"{args.target} from a terminal either, OR the ack "
                    "never made it out at the old rate (firmware <= "
                    "0.4.25 has the dead drain-wait; 0.4.26 fixes it).")
            elif any(d[1] > 0 for d in port.probe_deltas):
                log("VERDICT: SWITCH-FAILED -- the switch did not "
                    "hold, but the device IS alive and speaking JSON "
                    "at a probed rate without answering pings there "
                    "(see 'heard other JSON' above).")
            else:
                log("VERDICT: SWITCH-FAILED and no answer at any rate "
                    "after the revert window -- check the device is "
                    "powered/awake.")
            return 2

        # 4. Soak.
        sent = got = 0
        garbled_before = port.garbled
        soak_mark = port.mark()
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
        log(f"soak traffic: {Port.describe(port.delta(soak_mark))}")
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
            port.flush_line()
            if ping(port, timeout=2.0):
                port.send_line({"t": "baud_ok"})
                log(f"device was waiting at {args.restore} -- "
                    "baud_ok sent, left clean there")
            else:
                time.sleep(9.0)
                found = probe_rates(port, first=args.target)
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
