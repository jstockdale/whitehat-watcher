# Switchable serial baud for the tethered path — design, 2026-11-03

John's direction (2026-11-03): native Wi-Fi MQTT is the primary speed
track (Track B), AND the tethered (bridged USB, no Wi-Fi) mode must
work as well as possible. Transport is ~94% of the bridged PNG wall
(Step 0 measurements), and that transport is a 115200 UART. Baud
switching is the tethered-mode lever; it sequences BEFORE Track B.

## Facts (verified from source + schematic notes)

- The S3 data path is **UART0** (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`),
  carrying the newline-JSON link protocol on stdin/stdout
  (`firmware/components/link/link.c`).
- The board's bottom USB-C routes to a **dual USB-UART bridge**
  (schematic p.4, reviewed 2026-09-22): channel A = S3 UART0
  (console), channel B = Himax UART.
- Channel B runs **921600 baud reliably today** (every Himax XMODEM
  flash) — the bridge silicon, cables, and host drivers in this setup
  sustain the top rung on at least one channel.
- The exact bridge part is NOT yet identified (not in schematic text
  or host tooling). Dual-bridge parts of this class (e.g. CP2105)
  commonly top out at 921600. The design tolerates any ceiling via
  the ladder; identification is verification step 0, not a blocker.
- Firmware has 115200-derived timing constants that must become
  baud-derived: link.c TX-drain delays (~lines 283, 445), the RX
  arrival estimate (~line 2277), main.c's per-chunk timeout comment
  (line 238). Audit all of them in the implementation pass.
- The console page already has precedent: a separate Flash-baud
  setting, and the connection baud currently resets to 115200 on
  every page load. This design extends the connection side.

## Rulings (John, 2026-11-03)

- **Boot at 115200, always.** A known connect speed on every boot,
  every host, no state to go stale.
- **Rate ladder: 115200 / 230400 / 460800 / 921600.** Aim for the
  highest; fall back to lower rungs automatically if there are
  issues.
- **Console setting**: users can set their preferred speed in the
  Watcher Console (lands via the normal batched console-version
  approval process).

## Protocol

1. Host (console page) connects at 115200 as today.
2. Host sends a sealed command `{"t":"baud","rate":<rung>}` (control
   class; routine commander scope is sufficient — it changes comms
   state, not device authority).
3. Device replies (ack) at 115200, drains its TX ring
   (`uart_wait_tx_done`), then calls `uart_set_baudrate(UART_NUM_0)`.
4. Host closes/reopens its port at the new rate (WebSerial requires
   close+open; pyserial hosts can re-baud live) and sends a confirm
   ping.
5. **Auto-revert watchdog (device side)**: if no valid line arrives
   within 5 s of the switch, the device returns to 115200 and logs
   the revert. A botched switch can never strand the link. The host
   mirrors this: no confirm within its window → it also drops to
   115200 and re-handshakes.
6. Negotiation policy: starting rung = the console setting (Auto =
   921600). On switch failure, step down one rung and retry; the
   session's proven rate is remembered by the page for reconnects
   (device still boots at 115200 next time).
7. Mid-session stepdown: if a transfer at the current rung shows
   link errors (chunk CRC failures / framing garbage above a small
   threshold), the host drops one rung (repeat the switch protocol)
   rather than failing the transfer. 115200 is the floor — errors
   there are reported, not retried downward.

## Expected impact (bridged snaptest, 1.37 MB frame)

Serial chunk phase today ≈ 150 s at 115200. Scales with baud:
230400 ≈ 75 s, 460800 ≈ 38 s, 921600 ≈ 19 s. At the top rung the
tethered snaptest total approaches ~40 s (encode ~9 s + SPI ~4 s +
wire ~19 s + overhead), vs ~190–260 s today. No interaction with
native MQTT; B2's "skip the USB ring when native is up" is
unaffected — this work is purely the tethered path.

## Console changes (batched, approval-gated as usual)

- Setup: "Serial speed" setting — Auto / 115200 / 230400 / 460800 /
  921600 (persisted in localStorage like other prefs).
- Status surface: current negotiated rate shown next to the serial
  status; stepdown events logged to the page's event stream.
- The negotiation runs automatically after each connect.

## Verification plan

0. Identify the bridge part from host USB enumeration (VID/PID on
   the bench Tab) and record its rated max baud here.
1. Per-rung soak: at each of 230400/460800/921600 — snaptest must be
   byte-exact, spitest clean, zero chunk CRC failures across 5
   frames per rung.
2. Revert test: kill the host mid-switch; device must be back at
   115200 within ~5 s and accept a fresh console connect with no
   power cycle.
3. Stepdown test: force errors at 921600 (marginal cable) and watch
   the ladder land on a clean rung without losing the transfer.
4. Regression: Himax flash flow (other bridge channel) untouched;
   provisioning-over-serial still at 115200 boot state.

## Sequencing

This first, then Track B (B1 mirror-queue fix BEFORE any native
chunk traffic, per the unified plan). Track C (encode CPU) gains
importance only after both transports land.
