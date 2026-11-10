
#include "ma_transport_spi.h"

#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <new>

extern "C" {
#include <WE2_core.h>
#include <WE2_device_addr.h>
#include <console_io.h>
#include <hx_drv_gpio.h>
#include <hx_drv_scu.h>
#include <hx_drv_spi.h>
}

/* v0.4.20.45 (Claude §2): The SDK already has a HardFault_Handler in
 * app/scenario_app/sscma/hardfault_handler.c which prints via printf.
 * We do NOT define our own (would conflict). The g_fault_dump array
 * below can be populated by enhancing that handler if needed. */
__attribute__((section(".noinit"))) volatile uint32_t g_fault_dump[8];

#include <porting/ma_osal.h>
#include <porting/ma_misc.h>

#include <core/utils/ma_ringbuffer.hpp>
#include <core/ma_debug.h>

#include "ma_config_board.h"

#define SPI_PACKET_SIZE                 256
#define SPI_HEADER_LEN                  4
#define SPI_MAX_PL_LEN                  250
#define SPI_CHECKSUM_LEN                2

#define FEATURE_TRANSPORT               0x10
#define FEATURE_TRANSPORT_CMD_READ      0x01
#define FEATURE_TRANSPORT_CMD_WRITE     0x02
#define FEATURE_TRANSPORT_CMD_AVAILABLE 0x03
#define FEATURE_TRANSPORT_CMD_START     0x04
#define FEATURE_TRANSPORT_CMD_STOP      0x05
#define FEATURE_TRANSPORT_CMD_RESET     0x06
/* v0.4.14 (P1): CRC-protected bulk read. Like CMD_READ but stages
 * [data][CRC16-CCITT] and the S3 clocks len+2 bytes. */
#define FEATURE_TRANSPORT_CMD_READ_CRC  0x07
/* v0.4.20.11: SPI channel test. S3 sends len + pattern_id; Himax generates
 * a deterministic pattern, stages [pattern][CRC16], S3 clocks len+2 bytes
 * and verifies both data integrity and CRC. Isolates SPI issues from
 * camera capture complexity. */
#define FEATURE_TRANSPORT_CMD_TEST_PATTERN 0x08
/* v0.4.20.54 (Claude PARK spec): Park the MISO flag for SD card use.
 * No-response command. Handler re-arms RX but SKIPS the GPIO13 mux,
 * leaving PB2 released (tristate, S3 pull-up → HIGH). The Himax stays
 * RX-armed and silent; SD transactions can safely use SPI2. Exit is
 * implicit: the next command's normal handler restores GPIO13-LOW. */
#define FEATURE_TRANSPORT_CMD_PARK      0x09

/* v0.4.20.62 (link-hardening spec §5.6): injection hooks for the WI-4/
 * WI-5 bench tests. COMPILE-TIME ONLY, DEFAULT OFF — the release build
 * contains none of this (the shipped .62 is built with the switch off
 * and verified so). With the switch on, transport cmd 0x0A selects an
 * injection via the len field: 1 = fail the next spi_open once,
 * 2 = park the transport in the H-B unclaimed state (open, RX unarmed,
 * all claim flags false), 3 = stall the AT loop 60 s after the next
 * non-hook command is parsed (the H2 simulation). */
#ifndef WATCHER_LINK_TEST_HOOKS
#define WATCHER_LINK_TEST_HOOKS 0
#endif
#if WATCHER_LINK_TEST_HOOKS
#define FEATURE_TRANSPORT_CMD_TEST_HOOK 0x0A
static volatile uint32_t _test_open_fails     = 0;
static volatile bool     _test_stall_pending  = false;
static volatile bool     _test_stall_cmd_seen = false;
#endif

/* v0.4.14 (P1): CRC16-CCITT (poly 0x1021, init 0xFFFF). Mirrored on the
 * S3 (sscma_client_io_spi.c). Covers S3->Himax command packets
 * (header+payload) and Himax->S3 bulk data. */
static uint16_t spi_crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* v0.4.14 (P1): bulk feeder state. AT+SPIDUMP captures a frame and calls
 * ma::spi_bulk_prepare() with the capture buffer; the S3's CMD_START arms
 * the feeder, which pushes the buffer into the TX ring from ma::spi_poll()
 * (AT-server thread; the ring is SPSC with the SPI ISR as consumer).
 * CMD_STOP / CMD_RESET disarm. _bulk_gen invalidates a stale arm. */
static volatile bool     _spi_bulk_armed  = false;
static volatile uint32_t _spi_bulk_gen    = 0;
/* v0.4.20.23: bulk feeder watchdog timestamp (us). Updated on CMD_START
 * and every CMD_READ_CRC. If the feeder is armed but no READ_CRC arrives
 * within 1s, disarm automatically. This handles the case where the S3's
 * CMD_RESET is lost (Himax hadn't re-armed RX yet due to the wall-clock
 * TX→RX delay) — without this, the feeder keeps pushing frame data into
 * the TX ring while the S3 is back in AT mode, causing the S3's AT
 * parser to flood ("rx buffer is full" x20). */
static volatile int64_t  _spi_bulk_last_activity_us = 0;
static const uint8_t*    _spi_bulk_buf    = nullptr;
static size_t            _spi_bulk_len    = 0;
static size_t            _spi_bulk_offset = 0;
/* v0.4.14 (P1): count of command packets dropped for CRC mismatch. */
static volatile uint32_t _spi_crc_errors  = 0;

namespace ma {

static SPSCRingBuffer<char>* _spi_rb_rx  = nullptr;
static SPSCRingBuffer<char>* _spi_rb_tx  = nullptr;
static uint8_t*              _spi_rx_buf = nullptr;
static uint8_t*              _spi_tx_buf = nullptr;
static Mutex                 _spi_tx_mutex;
static DEV_SPI_PTR           _spi_dev    = nullptr;
static volatile bool         _spi_opened = false;
/* v0.4.12 (A-4): removed _spi_rx_blocking/_spi_tx_blocking — write-only,
 * no logic ever read them. */
/* v0.4.6.1: set when _spi_arm_rx()'s spi_read_dma fails. Retried from
 * thread context via ma::spi_poll() (called in the AT-server main loop).
 * The v0.4.6 approach of retrying inside _spi_rx_callback was dead code:
 * a failed arm leaves RX disarmed, so no callback can fire to consume it. */
static volatile bool         _spi_rx_arm_failed = false;
/* v0.4.20.62 (WI-5 H-B): RX-armed tracker. Set by a successful
 * spi_read_dma in either arm path; cleared when the RX DMA completes
 * (entry of _spi_rx_callback — completion is what disarms RX), when
 * the transport is cycled, and at deInit. Deliberately independent of
 * the staged/done/arm_failed bookkeeping: H-B exists to catch states
 * those three flags fail to claim, so it must not trust them. */
static volatile bool         _spi_rx_armed = false;
/* v0.4.20.54 (Claude PARK): parked flag. When true, the MISO flag (PB2) is
 * released (tristate) instead of driven LOW. Set by CMD_PARK, cleared by
 * any subsequent command's normal handler (which restores GPIO13-LOW). */
static volatile bool         _spi_parked = false;
/* v0.4.6.1: bytes peeked from the TX ring awaiting DMA completion.
 * Popped only in _spi_tx_callback on success (Fix 2: never lose data
 * on a failed TX arm). */
static volatile size_t       _spi_tx_pending_len = 0;
/* v0.4.20.29: separate pop length for READ_CRC. The _spi_tx_pending_len
 * is the DMA transfer length (data + CRC). But the ring pop must be
 * data bytes ONLY (the CRC was computed, not peeked from the ring).
 * For READ_CRC, pop_len = len; pending_len = len+2. For other commands,
 * they are equal. */
static volatile size_t       _spi_tx_pop_len = 0;
/* v0.4.20.7: defer RX re-arm after TX. The _spi_tx_callback fires when the
 * TX DMA feeds the SSI FIFO, BEFORE the S3 clocks the bytes out. Re-arming
 * RX immediately (the old behavior) switches the DW SPI slave to RX mode
 * while the S3 is still clocking the TX response, corrupting both the TX
 * data (large transfers read garbage → CRC mismatch) and the RX stream
 * (the "2-byte spill" where S3 response-read clocks land in the RX buffer).
 * Instead, the TX callback sets _spi_tx_done; ma::spi_poll() (thread
 * context, ~1kHz) re-arms RX after the S3 has finished clocking. */
static volatile bool         _spi_tx_done = false;
/* v0.4.20.52 (Claude Q4): _spi_tx_staged — set in the command handler when
 * spi_write_dma returns OK (response staged, PB2 about to go HIGH), cleared
 * in the drained path and in every teardown. Unlike _spi_tx_done (set by the
 * DMA-completion callback, which never fires if the S3 abandons a large
 * response mid-block), _spi_tx_staged covers the abandoned-response case of
 * any size. The 300ms staged watchdog is gated on this flag. */
static volatile bool         _spi_tx_staged = false;
static volatile int64_t      _spi_tx_staged_us = 0;
/* v0.4.20.52 (Claude Q4): arm-failure escalation counter. Counts consecutive
 * _spi_arm_rx() failures; teardown after ~10. Counter, not wall clock. */
static volatile uint32_t     _spi_rx_arm_fail_count = 0;
/* v0.4.20.52 (Claude Q1): cached SSI register base from
 * spi_control(SPI_CMD_GET_SPI_BASE_REG). Single source of truth for any
 * future register reads. */
static uint32_t              _spi_ssi_base_cached = 0;
/* v0.4.20.20: wall-clock TX completion timestamp (us). Replaces the
 * v0.4.20.17/19 poll-countdown, which assumed a ~20us poll period that
 * doesn't exist (the AT-server loop is yield-based with unmeasured
 * period). Using ma_get_time_us() is robust against loop timing
 * variations. Required wait = transfer time at 12MHz + 2ms margin. */
static volatile int64_t      _spi_tx_done_us = 0;
static volatile uint32_t     _spi_tx_len_for_timing = 0;

/* v0.4.20.41 (Phase A1): Status-based re-arm watchdog counter.
 * Firing = S3 abandoned a response (never clocked it out). */
static volatile uint32_t     _spi_wd_aborts = 0;
/* SSI base address for DW SPI slave 0 (HX6538 datasheet).
 * TXFLR (base+0x20): TX FIFO level; SR (base+0x28) bit0: BUSY. */
/* v0.4.20.42: Use ALIAS (non-secure) for thread-mode register access.
 * BASE_ADDR_AHB_SPI_S (0x40810000) is the secure view; the AT thread
 * needs the non-secure alias to avoid a bus fault. */
#define SSI_BASE_ADDR BASE_ADDR_AHB_SPI_S_ALIAS
#define SSI_TXFLR_OFFSET 0x20
#define SSI_SR_OFFSET 0x28
#define SSI_SR_BUSY_BIT 0x1

/* v0.4.6.1: ISR-safe diagnostic deferral (Fix 1).
 * MA_LOGE/printf MUST NOT run in the SPI DMA callbacks: they execute
 * synchronously inside the IRQ handler, and newlib's stdio lock traps
 * (Error_Handler + infinite loop) when __get_IPSR() != 0. Instead the
 * callbacks record a small diagnostic here; ma::spi_poll() prints it
 * from the AT-server thread context. */
struct SpiDiagRecord {
    volatile bool     pending;
    volatile uint8_t  where;  // 1=READ tx arm, 2=AVAILABLE tx arm, 3=RX arm
    volatile int32_t  rc;
    volatile uint32_t len;
};
static SpiDiagRecord _spi_diag = {false, 0, 0, 0};

/* v0.4.20.13: Deferred CMD_READ_CRC diagnostic. Records the requested len,
 * ring buffer avail, staged len (after clamping), first/last 4 bytes, and
 * CRC. Printed from spi_poll() thread context. */
struct SpiReadCrcDiag {
    volatile bool     pending;
    volatile uint16_t req_len;    // what S3 requested
    volatile uint32_t avail;      // ring buffer size at time of command
    volatile uint16_t staged_len; // what was actually staged (after clamp)
    volatile uint8_t  first4[4];
    volatile uint8_t  last4[4];
    volatile uint16_t crc;
};
static SpiReadCrcDiag _spi_readcrc_diag = {false, 0, 0, 0, {0}, {0}, 0};

/* v0.4.20.55: Non-blocking DIAG ring buffer. The SPI fast path formats
 * DIAG into this ring (memcpy, non-blocking). ma::spi_poll() drains it
 * to UART when the SPI link is idle, so bulk transfers aren't stalled
 * by 115200-baud UART TX. 16KB holds ~200 DIAG lines; the bulk generates
 * ~50KB over 36s (~1.4KB/s avg), well within drain capacity. */
#define DIAG_RING_SIZE 16384
static char _diag_ring[DIAG_RING_SIZE];
static volatile size_t _diag_ring_head = 0;  /* write index */
static volatile size_t _diag_ring_tail = 0;  /* read index */

/* Non-blocking DIAG emit. Formats into the ring, drops if full (should
 * not happen with 16KB and 1.4KB/s avg — indicates a bug if it does). */
static void diag_nb_emit(const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (len <= 0 || len >= (int)sizeof(tmp)) return;
    /* Need len+1 for newline; check space (leave 1 byte to distinguish full/empty) */
    size_t head = _diag_ring_head;
    size_t tail = _diag_ring_tail;
    size_t used = (head >= tail) ? (head - tail) : (DIAG_RING_SIZE - tail + head);
    size_t avail = DIAG_RING_SIZE - used - 1;
    if ((size_t)len + 1 > avail) return;  /* drop if full */
    for (int i = 0; i < len; i++) {
        _diag_ring[head] = tmp[i];
        head = (head + 1) % DIAG_RING_SIZE;
    }
    _diag_ring[head] = '\n';
    head = (head + 1) % DIAG_RING_SIZE;
    _diag_ring_head = head;
}

/* Drain DIAG ring to UART. Called from ma::spi_poll() when SPI is idle.
 * Writes in small chunks to avoid long blocking; returns when ring empty
 * or SPI becomes busy.
 * v0.4.20.56 (Claude F6): Capped at 32 chars/pass (~2.8ms at 115200 baud)
 * to avoid UART jitter affecting Guard timing/feeder. Also gated on
 * !_spi_bulk_armed to keep bulk transfers free of UART interference. */
static void diag_nb_drain(void) {
    /* Only drain if SPI is not in a transfer (avoid blocking the fast path).
     * Check _spi_tx_staged: if true, a TX is pending, don't block.
     * Also skip during bulk (feeder timing is sensitive). */
    if (_spi_tx_staged || _spi_bulk_armed) return;
    size_t tail = _diag_ring_tail;
    size_t head = _diag_ring_head;
    /* Drain up to 32 bytes per poll iteration. The poll runs at 1kHz,
     * so this keeps up with 1.4KB/s avg without 22ms stalls. */
    for (int i = 0; i < 32; i++) {
        if (tail == head) break;
        char c = _diag_ring[tail];
        ma_printf("%c", c);
        tail = (tail + 1) % DIAG_RING_SIZE;
        if (_spi_tx_staged || _spi_bulk_armed) break;
    }
    _diag_ring_tail = tail;
}

/* v0.4.20.58 (A0): TX-drain signal probes. In .58 they were
 * instrumentation only (Guard 1 re-armed on the 25/30ms timer).
 * v0.4.20.59: the probes DRIVE Guard 1 — see _rearm_signals_drained()
 * and the Guard 1 block in spi_poll(). While _spi_tx_done is pending, spi_poll()
 * samples the driver's status commands (driver-mediated — NOT the raw
 * MMIO that bus-faulted in v0.4.20.41/.43) and records when each first
 * reports "drained" relative to _spi_tx_done_us (the DMA-completion
 * stamp Guard 1 times from). A0's signal choice will be made from the
 * REARM_STATS evidence produced here. A probe whose spi_control
 * returns an error disables itself after one DIAG note (the prebuilt
 * driver may not implement every command for the slave device). */
struct RearmProbe {
    bool     active;
    uint32_t len;
    int32_t  t_first_poll_us;
    int32_t  t_busy_us;      /* GET_BUSY_STATUS first reads idle; -1 pending */
    int32_t  t_txavail_us;   /* TXAVAIL first >= capacity; -1 pending */
    int32_t  t_intdone_us;   /* INT_STATUS bit11 first set; -1 pending */
    int32_t  t_sr_us;        /* STATUS_REG BUSY first clear; -1 pending */
    bool     busy_seen;      /* BUSY read as busy at least once */
    bool     int_preset;     /* bit11 already set at the first sample */
    int32_t  txavail_first;  /* TXAVAIL at first sample */
};
static RearmProbe _rearm_probe = {false, 0, 0, -1, -1, -1, -1, false, false, -1};
static bool    _probe_dead_busy = false;
static bool    _probe_dead_txavail = false;
static bool    _probe_dead_int = false;
static bool    _probe_dead_sr = false;
static int32_t _probe_txavail_capacity = -1;  /* learned post-re-arm (FIFO empty) */

struct RearmStats {
    uint32_t responses;
    uint32_t abandoned;
    uint32_t signal_arms;     /* v0.4.20.59: armed on drained signals */
    uint32_t fallback_arms;   /* v0.4.20.59: armed by the 25/30ms ceiling */
    int32_t  busy_max_us;
    int32_t  txavail_max_us;
    int32_t  intdone_max_us;
    int32_t  sr_max_us;
    int32_t  arm_max_us;      /* slowest signal-arm latency */
    int32_t  fp_max_us;       /* worst first-poll latency (poll starvation) */
    uint32_t nc_busy;         /* per-signal still-pending at a fallback arm */
    uint32_t nc_txavail;
    uint32_t nc_int;
    uint32_t nc_sr;
    uint32_t busy_never_seen;
    uint32_t int_preset_count;
};
static RearmStats _rearm_stats = {0, 0, 0, 0, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0};
static uint32_t _rearm_since_emit = 0;
static uint32_t _rearm_verbatim_budget = 4;
static uint32_t _rearm_fb_verbatim_budget = 4;

static void _rearm_stats_emit(const char* why) {
    diag_nb_emit("I REARM %s n=%lu ab=%lu sig=%lu fb=%lu ncb=%lu nct=%lu nci=%lu ncs=%lu cap=%ld dead=%d%d%d%d wd=%lu",
            why,
            (unsigned long)_rearm_stats.responses,
            (unsigned long)_rearm_stats.abandoned,
            (unsigned long)_rearm_stats.signal_arms,
            (unsigned long)_rearm_stats.fallback_arms,
            (unsigned long)_rearm_stats.nc_busy,
            (unsigned long)_rearm_stats.nc_txavail,
            (unsigned long)_rearm_stats.nc_int,
            (unsigned long)_rearm_stats.nc_sr,
            (long)_probe_txavail_capacity,
            _probe_dead_busy ? 1 : 0, _probe_dead_txavail ? 1 : 0,
            _probe_dead_int ? 1 : 0, _probe_dead_sr ? 1 : 0,
            (unsigned long)_spi_wd_aborts);
    diag_nb_emit("I REARM_LAT bmax=%ld tmax=%ld imax=%ld smax=%ld amax=%ld fpmax=%ld",
            (long)_rearm_stats.busy_max_us, (long)_rearm_stats.txavail_max_us,
            (long)_rearm_stats.intdone_max_us, (long)_rearm_stats.sr_max_us,
            (long)_rearm_stats.arm_max_us, (long)_rearm_stats.fp_max_us);
    _rearm_stats = {0, 0, 0, 0, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0};
    _rearm_since_emit = 0;
}

static void _rearm_probe_sample() {
    if (!_spi_dev) return;
    int32_t dt = (int32_t)(ma_get_time_us() - _spi_tx_done_us);
    if (!_rearm_probe.active) {
        _rearm_probe.active = true;
        _rearm_probe.len = _spi_tx_len_for_timing;
        _rearm_probe.t_first_poll_us = dt;
        _rearm_probe.t_busy_us = -1;
        _rearm_probe.t_txavail_us = -1;
        _rearm_probe.t_intdone_us = -1;
        _rearm_probe.t_sr_us = -1;
        _rearm_probe.busy_seen = false;
        _rearm_probe.int_preset = false;
        _rearm_probe.txavail_first = -1;
    }
    if (!_probe_dead_busy && _rearm_probe.t_busy_us < 0) {
        int32_t v = 0;
        int32_t rc = _spi_dev->spi_control(SPI_CMD_GET_BUSY_STATUS, (void*)&v);
        if (rc != E_OK) {
            _probe_dead_busy = true;
            diag_nb_emit("E REARM probe BUSY dead rc=%ld", (long)rc);
        } else if (v != 0) {
            _rearm_probe.busy_seen = true;
        } else {
            _rearm_probe.t_busy_us = dt;
        }
    }
    if (!_probe_dead_txavail) {
        int32_t v = -1;
        int32_t rc = _spi_dev->spi_control(SPI_CMD_GET_TXAVAIL, (void*)&v);
        if (rc != E_OK) {
            _probe_dead_txavail = true;
            diag_nb_emit("E REARM probe TXAVAIL dead rc=%ld", (long)rc);
        } else {
            if (_rearm_probe.txavail_first < 0) _rearm_probe.txavail_first = v;
            if (_rearm_probe.t_txavail_us < 0 &&
                _probe_txavail_capacity > 0 && v >= _probe_txavail_capacity) {
                _rearm_probe.t_txavail_us = dt;
            }
        }
    }
    if (!_probe_dead_int && _rearm_probe.t_intdone_us < 0) {
        int32_t v = 0;
        int32_t rc = _spi_dev->spi_control(SPI_CMD_GET_INT_STATUS, (void*)&v);
        if (rc != E_OK) {
            _probe_dead_int = true;
            diag_nb_emit("E REARM probe INT dead rc=%ld", (long)rc);
        } else if (v & (1 << 11)) {
            _rearm_probe.t_intdone_us = dt;
            if (dt == _rearm_probe.t_first_poll_us) _rearm_probe.int_preset = true;
        }
    }
    if (!_probe_dead_sr && _rearm_probe.t_sr_us < 0) {
        uint32_t v = 0;
        int32_t rc = _spi_dev->spi_control(SPI_CMD_GET_STATUS_REG, (void*)&v);
        if (rc != E_OK) {
            _probe_dead_sr = true;
            diag_nb_emit("E REARM probe SR dead rc=%ld", (long)rc);
        } else if ((v & SSI_SR_BUSY_BIT) == 0) {
            _rearm_probe.t_sr_us = dt;
        }
    }
}

/* v0.4.20.59: the drained predicate for signal-driven re-arm.
 * BUSY idle AND SR BUSY clear AND TXAVAIL back at capacity. A dead
 * probe abstains; TXAVAIL abstains until capacity is learned (the
 * .58 boot edge). INT_STATUS bit11 is excluded from the gate — it
 * never asserted in 11,408 .58 responses. TXAVAIL is the load-bearing
 * member for small responses: bytes the S3 has not clocked yet keep
 * it below capacity, so the gate cannot open early on an un-clocked
 * response (the .20-era corruption mode). */
static bool _rearm_signals_drained() {
    if (!_probe_dead_busy && _rearm_probe.t_busy_us < 0) return false;
    if (!_probe_dead_sr && _rearm_probe.t_sr_us < 0) return false;
    if (!_probe_dead_txavail && _probe_txavail_capacity > 0 &&
        _rearm_probe.t_txavail_us < 0) return false;
    return true;
}

static void _rearm_probe_finish(bool signal_armed, int32_t arm_elapsed_us) {
    if (!_rearm_probe.active) return;
    _rearm_probe.active = false;
    _rearm_stats.responses++;
    if (_rearm_probe.t_first_poll_us > _rearm_stats.fp_max_us) {
        _rearm_stats.fp_max_us = _rearm_probe.t_first_poll_us;
    }
    if (signal_armed) {
        _rearm_stats.signal_arms++;
        if (arm_elapsed_us > _rearm_stats.arm_max_us) {
            _rearm_stats.arm_max_us = arm_elapsed_us;
        }
    } else {
        /* Fallback arm (timer ceiling): record which signals were
         * still pending — the per-signal not-clear counts. */
        _rearm_stats.fallback_arms++;
        if (!_probe_dead_busy && _rearm_probe.t_busy_us < 0) _rearm_stats.nc_busy++;
        if (!_probe_dead_txavail && _rearm_probe.t_txavail_us < 0) _rearm_stats.nc_txavail++;
        if (!_probe_dead_int && _rearm_probe.t_intdone_us < 0) _rearm_stats.nc_int++;
        if (!_probe_dead_sr && _rearm_probe.t_sr_us < 0) _rearm_stats.nc_sr++;
    }
    if (!_probe_dead_busy) {
        if (_rearm_probe.t_busy_us >= 0 &&
            _rearm_probe.t_busy_us > _rearm_stats.busy_max_us) {
            _rearm_stats.busy_max_us = _rearm_probe.t_busy_us;
        }
        if (!_rearm_probe.busy_seen) _rearm_stats.busy_never_seen++;
    }
    if (!_probe_dead_txavail) {
        if (_rearm_probe.t_txavail_us >= 0 &&
            _rearm_probe.t_txavail_us > _rearm_stats.txavail_max_us) {
            _rearm_stats.txavail_max_us = _rearm_probe.t_txavail_us;
        }
    }
    if (!_probe_dead_int) {
        if (_rearm_probe.t_intdone_us >= 0 &&
            _rearm_probe.t_intdone_us > _rearm_stats.intdone_max_us) {
            _rearm_stats.intdone_max_us = _rearm_probe.t_intdone_us;
        }
        if (_rearm_probe.int_preset) _rearm_stats.int_preset_count++;
    }
    if (!_probe_dead_sr) {
        if (_rearm_probe.t_sr_us >= 0 &&
            _rearm_probe.t_sr_us > _rearm_stats.sr_max_us) {
            _rearm_stats.sr_max_us = _rearm_probe.t_sr_us;
        }
    }
    /* v0.4.20.59: verbatim budget is a hard budget (the .58 rule —
     * emit on every not-clear — flooded one line per response). */
    bool verbatim = false;
    if (_rearm_verbatim_budget > 0) {
        _rearm_verbatim_budget--;
        verbatim = true;
    } else if (!signal_armed && _rearm_fb_verbatim_budget > 0) {
        _rearm_fb_verbatim_budget--;
        verbatim = true;
    }
    if (verbatim) {
        diag_nb_emit("I REARM_ONE len=%lu fp=%ld b=%ld t=%ld i=%ld s=%ld bs=%d ip=%d taf=%ld",
                (unsigned long)_rearm_probe.len,
                (long)_rearm_probe.t_first_poll_us,
                (long)_rearm_probe.t_busy_us, (long)_rearm_probe.t_txavail_us,
                (long)_rearm_probe.t_intdone_us, (long)_rearm_probe.t_sr_us,
                _rearm_probe.busy_seen ? 1 : 0, _rearm_probe.int_preset ? 1 : 0,
                (long)_rearm_probe.txavail_first);
    }
    _rearm_since_emit++;
    if (!_spi_bulk_armed && _rearm_since_emit >= 32) {
        _rearm_stats_emit("periodic");
    }
}

/* Guard 2 / Guard 3 teardown paths clear _spi_tx_done without Guard 1
 * firing (abandoned response): close out the probe without latencies. */
static void _rearm_probe_drop() {
    if (_rearm_probe.active) {
        _rearm_probe.active = false;
        _rearm_stats.abandoned++;
    }
}

static inline void _spi_diag_record(uint8_t where, int32_t rc, uint32_t len) {
    _spi_diag.where = where;
    _spi_diag.rc    = rc;
    _spi_diag.len   = len;
    _spi_diag.pending = true;  // set last
}

static void _spi_rx_callback(void* param);
static void _spi_tx_callback(void* param);
/* v0.4.20.62: defined beside spi_poll(); the TEST_HOOK injection in
 * _spi_rx_callback needs it before that definition point. */
static bool _spi_transport_cycle();

static void _spi_arm_rx() {
    if (!_spi_dev) {
        return;
    }
    if (!_spi_opened) {
        /* v0.4.20.62 (WI-4 H-A(1)): never fail silently. A closed
         * transport with RX unarmed is the incident's end-state;
         * setting the flag hands the state to Guard 3, whose block
         * head now retries the open before the arm. (The !_spi_dev
         * case above stays silent: the transport was never
         * initialized and Guard 3 has nothing to escalate to.) */
        _spi_rx_arm_failed = true;
        return;
    }
    std::memset(_spi_rx_buf, 0, SPI_PACKET_SIZE);
    hx_CleanDCache_by_Addr((void*)_spi_rx_buf, SPI_PACKET_SIZE);
    /* v0.4.6.1: check the DMA arm return code. On failure record a
     * deferred diagnostic (no printf in ISR context) and flag a retry;
     * ma::spi_poll() retries the arm from thread context. */
    int32_t rc = _spi_dev->spi_read_dma(_spi_rx_buf, SPI_PACKET_SIZE, (void*)_spi_rx_callback);
    if (rc != E_OK) {
        _spi_diag_record(3, rc, SPI_PACKET_SIZE);
        _spi_rx_arm_failed = true;
        /* v0.4.20.40: DO NOT drive PB2 LOW if RX DMA failed! The S3
         * interprets LOW as "RX armed, safe to send". Driving LOW
         * when not armed causes the S3 to send commands into the void. */
        return;
    }
    _spi_rx_arm_failed = false;
    _spi_rx_armed = true;  /* v0.4.20.62 (WI-5 H-B tracker) */
    /* v0.4.20.38 (Phase 2): MISO-flag LOW = RX armed, safe to send command.
     * Mux PB2 to GPIO13 (driven LOW). This must happen AFTER RX DMA is armed. */
    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_GPIO13, 1);
    /* GPIO13 is already configured as output LOW from init; ensure it stays LOW. */
    hx_drv_gpio_set_out_value(GPIO13, GPIO_OUT_LOW);
    /* v0.4.20.54: clear parked flag — normal re-arm restores the flag. */
    _spi_parked = false;
}

/* v0.4.20.54 (Claude PARK): Parked re-arm. Arms RX DMA but SKIPS the
 * GPIO13 mux, leaving PB2 released (tristate). When deselected, the DW
 * slave tristates DO, so the S3's pull-up takes the line HIGH. The Himax
 * stays RX-armed and silent; SD can safely drive MISO. */
static void _spi_arm_rx_parked() {
    if (!_spi_dev) {
        return;
    }
    if (!_spi_opened) {
        /* v0.4.20.62 (WI-4 H-A(1)): same never-silent rule as
         * _spi_arm_rx — the spec's §5.2 invariant (no reachable state
         * with all claim flags false, RX unarmed, transport closed)
         * must hold for both arm entry points. */
        _spi_rx_arm_failed = true;
        return;
    }
    std::memset(_spi_rx_buf, 0, SPI_PACKET_SIZE);
    hx_CleanDCache_by_Addr((void*)_spi_rx_buf, SPI_PACKET_SIZE);
    int32_t rc = _spi_dev->spi_read_dma(_spi_rx_buf, SPI_PACKET_SIZE, (void*)_spi_rx_callback);
    if (rc != E_OK) {
        _spi_diag_record(3, rc, SPI_PACKET_SIZE);
        _spi_rx_arm_failed = true;
        return;
    }
    _spi_rx_arm_failed = false;
    _spi_rx_armed = true;  /* v0.4.20.62 (WI-5 H-B tracker) */
    /* v0.4.20.56 (Claude F4): PARKED must explicitly mux PB2 to SPI_S_DO.
     * The old comment assumed PB2 was "SPI_S_DO from the last TX", but
     * PARK arrives while PB2 is GPIO13-driven-LOW (that's how the command
     * passed the gate). Leaving it as-is would keep the line driven LOW,
     * defeating PARK's purpose (SD contention). Explicitly release to
     * SPI_S_DO; the deselected slave tristates DO, S3 pull-up takes HIGH. */
    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
    /* _spi_parked stays true — cleared by the next normal command. */
}

/* v0.4.20.60: CRC validation for a scan-realignment candidate at
 * offset o. Mirrors exactly what the handlers trust:
 * - CMD_WRITE: payload len <= SPI_MAX_PL_LEN and the packet CRC16
 *   trailer (over header+payload) matches — a 0xFFFF trailer or a
 *   trailer past the received bytes is the legacy/unreceivable case
 *   the WRITE handler itself accepts.
 * - Any other command: the header CRC16 at [o+4..o+5] over the 4
 *   header bytes matches — 0xFFFF is CMD_RESET's legacy filler. */
static bool _spi_frame_crc_ok(size_t o, uint8_t cmd, uint16_t len) {
    if (cmd == FEATURE_TRANSPORT_CMD_WRITE) {
        if (len > SPI_MAX_PL_LEN) {
            return false;
        }
        size_t crc_at = o + 4 + (size_t)len;
        if (crc_at + 2 > SPI_PACKET_SIZE) {
            return true;
        }
        uint16_t rx_crc = (uint16_t)(_spi_rx_buf[crc_at] << 8 |
                                     _spi_rx_buf[crc_at + 1]);
        if (rx_crc == 0xFFFF) {
            return true;
        }
        return rx_crc == spi_crc16_ccitt(&_spi_rx_buf[o], 4 + (size_t)len);
    }
    uint16_t rx_crc = (uint16_t)(_spi_rx_buf[o + 4] << 8 |
                                 _spi_rx_buf[o + 5]);
    if (rx_crc == 0xFFFF) {
        return true;
    }
    return rx_crc == spi_crc16_ccitt(&_spi_rx_buf[o], 4);
}

static void _spi_rx_callback(void* param) {
    (void)param;

    /* v0.4.20.62 (WI-5 H-B tracker): the RX DMA has completed, so RX
     * is unarmed from here until a handler re-arms (or stages a TX,
     * which the staged flag claims). */
    _spi_rx_armed = false;

    // DMA has written the received packet to _spi_rx_buf. Invalidate the DCache
    // so the CPU reads the DMA'd data, not stale cached lines.
    hx_InvalidateDCache_by_Addr((void*)_spi_rx_buf, SPI_PACKET_SIZE);

    uint8_t  feature = _spi_rx_buf[0];
    uint8_t  cmd     = _spi_rx_buf[1];
    uint16_t len     = (uint16_t)(_spi_rx_buf[2] << 8 | _spi_rx_buf[3]);

    /* v0.4.10: tolerate a 2-byte spill at the head of the RX stream.
     * When the TX DMA for a short response (<= SSI FIFO depth, e.g. the
     * 2-byte AVAILABLE count) completes before the S3 clocks it out, the TX
     * callback re-arms RX early and the S3's response-read clocks land in
     * the RX buffer ahead of the next command packet. Sniff for the
     * FEATURE_TRANSPORT magic at offset 0 (normal) or 2 (spilled).
     * v0.4.12 (A-2): this spill is a STABLE per-command offset, not a
     * one-time glitch — every AVAILABLE response re-arms RX early, so each
     * subsequent command arrives as [2 idle bytes + 254 command bytes].
     * Invariant: the last 2 bytes of every 256-byte command packet are
     * LOST. Harmless today (WRITE payload caps at 250; bytes 254-255 are
     * padding), but no future command may use bytes 254-255. Long-term,
     * consider S3-side full-duplex (TX cmd + RX response in one
     * transaction) for AVAILABLE.
     * v0.4.20.60: spills larger than 2 bytes are real (see the scan
     * below) — the offset-2 check is now just the fast legacy case of a
     * general CRC-validated scan over all offsets. */
    size_t cmd_off = 0;
    if (feature != FEATURE_TRANSPORT && _spi_rx_buf[2] == FEATURE_TRANSPORT) {
        uint8_t cmd_try = _spi_rx_buf[3];
        /* v0.4.14 (P1): accept CMD_READ_CRC (0x07) in the spill check.
         * v0.4.20.11: also accept CMD_TEST_PATTERN (0x08). */
        if (cmd_try >= FEATURE_TRANSPORT_CMD_READ && cmd_try <= FEATURE_TRANSPORT_CMD_PARK) {
            cmd_off = 2;
            feature = _spi_rx_buf[cmd_off + 0];
            cmd     = cmd_try;
            len     = (uint16_t)(_spi_rx_buf[cmd_off + 2] << 8 | _spi_rx_buf[cmd_off + 3]);
        }
    }

    /* v0.4.20.60: if neither offset 0 nor the legacy offset 2 matched,
     * scan the whole buffer for a CRC-valid frame header at any other
     * offset. The .59 fast re-arm exposed spills larger than 2 bytes
     * (drain-clock spill on a busy link at gap-0 cadence — 5 eaten
     * commands in one spitest cascade, 2026-11-02) that the fixed
     * offset-2 check could not realign, so the commands were dropped.
     * A candidate must carry the FEATURE_TRANSPORT magic, a command in
     * range, and a valid CRC (see _spi_frame_crc_ok) — the CRC is what
     * makes a deep offset trustworthy, and the scan only runs on frames
     * the legacy checks already rejected, so no frame accepted today
     * can be rejected by it. Recovery is recorded (diag where=10) so
     * spill sizes stay visible in the DIAG stream. */
    if (feature != FEATURE_TRANSPORT) {
        for (size_t o = 1; o + 6 <= SPI_PACKET_SIZE; o++) {
            if (o == 2) {
                continue;  /* already tried above (magic+cmd only) */
            }
            if (_spi_rx_buf[o] != FEATURE_TRANSPORT) {
                continue;
            }
            uint8_t cmd_try = _spi_rx_buf[o + 1];
            if (cmd_try < FEATURE_TRANSPORT_CMD_READ ||
                cmd_try > FEATURE_TRANSPORT_CMD_PARK) {
                continue;
            }
            uint16_t len_try = (uint16_t)(_spi_rx_buf[o + 2] << 8 |
                                          _spi_rx_buf[o + 3]);
            if (!_spi_frame_crc_ok(o, cmd_try, len_try)) {
                continue;
            }
            cmd_off = o;
            feature = FEATURE_TRANSPORT;
            cmd     = cmd_try;
            len     = len_try;
            _spi_diag_record(10, (int32_t)o, (uint32_t)cmd);
            break;
        }
    }

    if (feature != FEATURE_TRANSPORT) {
        /* v0.4.20.56: Record parser-drop diagnostic. Previously silent;
         * now we capture the offending bytes for post-mortem analysis.
         * Uses the deferred DIAG mechanism (printed from ma::spi_poll). */
        _spi_diag_record(9, (int32_t)_spi_rx_buf[0], (uint32_t)_spi_rx_buf[2]);
        _spi_arm_rx();
        return;
    }
    /* v0.4.12 (A-1): retire the previous response's TX-ring bytes NOW, at
     * the next valid command parse — not in _spi_tx_callback. The S3 bus
     * discipline is strictly sequential (command TX, then response-read
     * RX), so a new valid command proves the previous response was fully
     * clocked out. _spi_tx_callback fires when the DMA feeds the SSI
     * FIFO, BEFORE the S3 clocks the bytes out, so popping there silently
     * loses bytes whenever a response-read is short. (SPSC discipline
     * unchanged: pop still happens in IRQ context.) */
    if (_spi_tx_pending_len > 0) {
        /* v0.4.20.29: pop data bytes only, not CRC. _spi_tx_pop_len is
         * set by each command handler:
         * - CMD_READ: pop_len = len (data, no CRC)
         * - CMD_READ_CRC: pop_len = len (data); pending_len = len+2 (DMA)
         * - CMD_TEST_PATTERN: pop_len = 0 (generated, not from ring)
         * Popping len+2 for READ_CRC desyncs the ring by 2 bytes/chunk! */
        _spi_rb_tx->pop((char*)_spi_tx_buf, _spi_tx_pop_len);
        _spi_tx_pending_len = 0;
        _spi_tx_pop_len = 0;
    }
    /* v0.4.20.56 (Claude F3): Flag lifecycle consolidation. A new valid
     * command proves the previous response was fully clocked out (same
     * invariant as the ring pop above). Clear staged/done here,
     * unconditionally, as the single lifecycle point. The scattered
     * per-handler clears (STOP, START) are deleted. */
    _spi_tx_staged = false;
    _spi_tx_staged_us = 0;
    _spi_tx_done = false;
    _spi_tx_done_us = 0;
#if WATCHER_LINK_TEST_HOOKS
    /* Injection selector 3: a real command was parsed after the stall
     * was armed — spi_poll performs the stall on its next pass. */
    if (_test_stall_pending && cmd != FEATURE_TRANSPORT_CMD_TEST_HOOK) {
        _test_stall_cmd_seen = true;
    }
#endif
    /* v0.4.10: the len field means different things per command. For WRITE
     * it is the payload length (capped at SPI_MAX_PL_LEN=250). For READ it
     * is the number of response bytes the S3 wants clocked out (the S3
     * legitimately requests up to MAX_RECIEVE_SIZE=4095). The old blanket
     * `len > SPI_MAX_PL_LEN` rejection here silently dropped every READ
     * larger than 250 bytes — the boot INIT burst alone is ~500 B — so the
     * TX ring never drained, the S3 clocked MISO-idle fill, and no AT frame
     * ever parsed (no link_up). Validate per command below. */

    switch (cmd) {
    case FEATURE_TRANSPORT_CMD_WRITE:
        // S3 sent AT command text: push to RX ringbuffer for AT server.
        // v0.4.10: WRITE payload is capped at SPI_MAX_PL_LEN (250); also
        // clamp to the bytes actually present when cmd_off==2 spilled.
        if (len > SPI_MAX_PL_LEN) {
            _spi_arm_rx();
            break;
        }
        if (cmd_off + 4 + len > SPI_PACKET_SIZE) {
            len = SPI_PACKET_SIZE - cmd_off - 4;
        }
        /* v0.4.14 (P1): verify the CRC16 trailer when it is present and
         * receivable. The S3 sends CRC16-CCITT over [FEATURE, CMD, LEN_HI,
         * LEN_LO, payload...] at [cmd_off+4+len]. Legacy S3 firmware sends
         * 0xFF 0xFF (accepted without verification for interop). Under the
         * stable 2-byte spill (cmd_off==2) the packet's last 2 bytes are
         * lost, so skip verification when the CRC bytes fall in the lost
         * region. The S3 caps CRC-mode WRITE payloads at 248 bytes so the
         * CRC always survives. */
        {
            size_t crc_at = cmd_off + 4 + len;
            bool crc_receivable = (crc_at + 2 <= SPI_PACKET_SIZE);
            if (crc_receivable) {
                uint16_t rx_crc = (uint16_t)(_spi_rx_buf[crc_at] << 8 |
                                             _spi_rx_buf[crc_at + 1]);
                if (rx_crc != 0xFFFF) {
                    uint16_t calc =
                        spi_crc16_ccitt(&_spi_rx_buf[cmd_off], 4 + len);
                    if (rx_crc != calc) {
                        _spi_crc_errors++;
                        _spi_arm_rx();
                        break;
                    }
                }
            }
        }
        _spi_rb_rx->push((const char*)&_spi_rx_buf[cmd_off + 4], len);
        _spi_arm_rx();
        break;

    case FEATURE_TRANSPORT_CMD_READ: {
        // S3 wants to read response data.
        // v0.4.10: len here is a byte-count request (S3 MAX_RECIEVE_SIZE
        // = 4095), NOT a payload length — accept up to the TX staging
        // buffer size. Clamp to what's actually queued below.
        // v0.4.6.1 (Fix 2): PEEK from the TX ringbuffer — do NOT pop yet.
        // The bytes are popped only in _spi_tx_callback after the DMA
        // completes successfully. If the TX arm fails below, the data
        // stays in the ring for the S3's next READ (no silent loss).
        size_t avail = _spi_rb_tx->size();
        if (len > 4095) {
            len = 4095;  // v0.4.10: S3 MAX_RECIEVE_SIZE; _spi_tx_buf is 4096
        }
        if (len > avail) {
            len = avail;
        }
        if (len > 0) {
            _spi_rb_tx->peek((char*)_spi_tx_buf, len);
            _spi_tx_pending_len = len;
            /* v0.4.20.29: pop_len = len (data only, no CRC for CMD_READ) */
            _spi_tx_pop_len = len;
        } else {
            _spi_tx_pending_len = 0;
            _spi_tx_pop_len = 0;
        }
        hx_CleanDCache_by_Addr((void*)_spi_tx_buf, len > 0 ? len : 1);
        // Arm TX: S3 will clock out the data in its follow-up read transaction
        // We transmit len bytes; S3 may clock more (up to 4095), extra will be zeros
        // DO NOT re-arm RX here — the DW slave can only be armed for one direction.
        // RX will be re-armed in _spi_tx_callback after the TX completes.
        /* v0.4.6.1: check the TX arm return code. If the arm fails the TX
         * callback will never fire; record a deferred diagnostic (no
         * printf in ISR) and re-arm RX immediately to keep the link alive.
         * The peeked payload stays in the ring (see above). */
        int32_t tx_rc = _spi_dev->spi_write_dma(_spi_tx_buf, len > 0 ? len : 1, (void*)_spi_tx_callback);
        /* v0.4.20.38 (Phase 2): MISO-flag HIGH = response staged.
         * Mux PB2 to SPI_S_DO; the DW slave tristates DO when deselected,
         * so the S3's pull-up takes the line HIGH. This release IS the signal.
         * Only do this on success; on failure, _spi_arm_rx() will drive LOW. */
        if (tx_rc == E_OK) {
            _spi_tx_staged = true;
            _spi_tx_staged_us = ma_get_time_us();
            hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
        }
        if (tx_rc != E_OK) {
            _spi_diag_record(1, tx_rc, len);
            _spi_tx_pending_len = 0;
            _spi_arm_rx();
        }
        break;
    }

    case FEATURE_TRANSPORT_CMD_READ_CRC: {
        /* v0.4.14 (P1): CRC-protected bulk read. Like CMD_READ but stages
         * [data][CRC16-CCITT] and the S3 clocks len+2 bytes, verifying the
         * CRC per chunk. The peek/pop discipline matches CMD_READ: peek
         * now, pop the data bytes (not the CRC) at the next valid command
         * parse. len is clamped to 4094 so [data][CRC] fits the 4096-byte
         * staging buffer.
         * v0.4.20.13: record deferred diagnostic (req_len, avail,
         * staged_len, first/last bytes, CRC) for S3-side debugging.
         * v0.4.20.23: feed the bulk watchdog. */
        _spi_bulk_last_activity_us = ma_get_time_us();
        uint16_t req_len = len;
        size_t avail = _spi_rb_tx->size();
        if (len > 4094) {
            len = 4094;
        }
        if (len > avail) {
            len = avail;
        }
        if (len > 0) {
            _spi_rb_tx->peek((char*)_spi_tx_buf, len);
            uint16_t crc = spi_crc16_ccitt(_spi_tx_buf, len);
            _spi_tx_buf[len]     = (uint8_t)(crc >> 8);
            _spi_tx_buf[len + 1] = (uint8_t)(crc & 0xFF);
            /* v0.4.20.19: pending_len must equal the DMA length (len+2),
             * not len, so the poll-countdown covers the CRC bytes.
             * v0.4.20.29: pop_len = len (data only!). The CRC bytes were
             * computed, not peeked from the ring. Popping len+2
             * desyncs the ring by 2 bytes per chunk -> misaligned data
             * -> CRC fails on chunk 1+. */
            _spi_tx_pending_len = len + 2;
            _spi_tx_pop_len = len;
            /* v0.4.20.13: record diagnostic (ISR-safe, printed later).
             * v0.4.20.16: guard against len < 4 (buffer underflow crash). */
            _spi_readcrc_diag.req_len = req_len;
            _spi_readcrc_diag.avail = (uint32_t)avail;
            _spi_readcrc_diag.staged_len = len;
            if (len >= 4) {
                _spi_readcrc_diag.first4[0] = _spi_tx_buf[0];
                _spi_readcrc_diag.first4[1] = _spi_tx_buf[1];
                _spi_readcrc_diag.first4[2] = _spi_tx_buf[2];
                _spi_readcrc_diag.first4[3] = _spi_tx_buf[3];
                _spi_readcrc_diag.last4[0] = _spi_tx_buf[len-4];
                _spi_readcrc_diag.last4[1] = _spi_tx_buf[len-3];
                _spi_readcrc_diag.last4[2] = _spi_tx_buf[len-2];
                _spi_readcrc_diag.last4[3] = _spi_tx_buf[len-1];
            } else {
                _spi_readcrc_diag.first4[0] = 0;
                _spi_readcrc_diag.first4[1] = 0;
                _spi_readcrc_diag.first4[2] = 0;
                _spi_readcrc_diag.first4[3] = 0;
                _spi_readcrc_diag.last4[0] = 0;
                _spi_readcrc_diag.last4[1] = 0;
                _spi_readcrc_diag.last4[2] = 0;
                _spi_readcrc_diag.last4[3] = 0;
            }
            _spi_readcrc_diag.crc = crc;
            _spi_readcrc_diag.pending = true;
        } else {
            _spi_tx_pending_len = 0;
            _spi_readcrc_diag.req_len = req_len;
            _spi_readcrc_diag.avail = (uint32_t)avail;
            _spi_readcrc_diag.staged_len = 0;
            _spi_readcrc_diag.crc = 0;
            _spi_readcrc_diag.pending = true;
        }
        hx_CleanDCache_by_Addr((void*)_spi_tx_buf, len > 0 ? len + 2 : 1);
        int32_t crc_rc = _spi_dev->spi_write_dma(_spi_tx_buf, len > 0 ? len + 2 : 1, (void*)_spi_tx_callback);
        /* v0.4.20.38 (Phase 2): MISO-flag HIGH = response staged. */
        if (crc_rc == E_OK) {
            _spi_tx_staged = true;
            _spi_tx_staged_us = ma_get_time_us();
            hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
        }
        if (crc_rc != E_OK) {
            _spi_diag_record(1, crc_rc, len);
            _spi_tx_pending_len = 0;
            _spi_arm_rx();
        }
        break;
    }

    case FEATURE_TRANSPORT_CMD_TEST_PATTERN: {
        /* v0.4.20.11: SPI channel self-test. The S3 sends len (bytes) and
         * the pattern ID is encoded in the upper 2 bits of len:
         *   bits 15-14: pattern (0=zeros, 1=0xFF, 2=incrementing, 3=LCG pseudo-random)
         *   bits 13-0: length (0-16383, clamped to 4094)
         * Generates the pattern directly into _spi_tx_buf, computes CRC,
         * stages [pattern][CRC]. The S3 verifies both data integrity and
         * CRC, isolating SPI issues from camera capture. */
        uint8_t pattern_id = (uint8_t)((len >> 14) & 0x03);
        len &= 0x3FFF;
        if (len > 4094) {
            len = 4094;
        }
        if (len > 0) {
            uint32_t lcg_state = 12345;  /* v0.4.20.11: local, reset per command */
            for (size_t i = 0; i < len; i++) {
                switch (pattern_id) {
                    case 0: _spi_tx_buf[i] = 0x00; break;
                    case 1: _spi_tx_buf[i] = 0xFF; break;
                    case 2: _spi_tx_buf[i] = (uint8_t)(i & 0xFF); break;
                    case 3:
                        lcg_state = (1103515245u * lcg_state + 12345u) & 0x7FFFFFFFu;
                        _spi_tx_buf[i] = (uint8_t)((lcg_state >> 16) & 0xFF);
                        break;
                }
            }
            uint16_t crc = spi_crc16_ccitt(_spi_tx_buf, len);
            _spi_tx_buf[len]     = (uint8_t)(crc >> 8);
            _spi_tx_buf[len + 1] = (uint8_t)(crc & 0xFF);
            /* v0.4.20.19: pending_len must equal the DMA length (len+2). */
            _spi_tx_pending_len = len + 2;
            /* v0.4.20.29: TEST_PATTERN generates data directly, nothing
             * peeked from ring. Pop length = 0. */
            _spi_tx_pop_len = 0;
        } else {
            _spi_tx_pending_len = 0;
            _spi_tx_pop_len = 0;
        }
        hx_CleanDCache_by_Addr((void*)_spi_tx_buf, len > 0 ? len + 2 : 1);
        int32_t tp_rc = _spi_dev->spi_write_dma(_spi_tx_buf, len > 0 ? len + 2 : 1, (void*)_spi_tx_callback);
        /* v0.4.20.38 (Phase 2): MISO-flag HIGH = response staged. */
        if (tp_rc == E_OK) {
            _spi_tx_staged = true;
            _spi_tx_staged_us = ma_get_time_us();
            hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
        }
        if (tp_rc != E_OK) {
            _spi_diag_record(1, tp_rc, len);
            _spi_tx_pending_len = 0;
            _spi_arm_rx();
        }
        break;
    }

    case FEATURE_TRANSPORT_CMD_AVAILABLE: {
        // S3 asks how many response bytes are available: return 2-byte big-endian count
        uint16_t available = (uint16_t)_spi_rb_tx->size();
        _spi_tx_buf[0] = (available >> 8) & 0xFF;
        _spi_tx_buf[1] = available & 0xFF;
        hx_CleanDCache_by_Addr((void*)_spi_tx_buf, 2);
        /* v0.4.20.35: MUST set _spi_tx_pending_len! The _spi_tx_callback
         * copies this to _spi_tx_len_for_timing for the adaptive timeout.
         * Without this, it holds the stale value from the previous command
         * (e.g., 4092 from READ_CRC), causing a 5226us timeout instead of
         * 1501us -> Himax can't receive the next READ_CRC -> S3 gets FFs. */
        _spi_tx_pending_len = 2;
        _spi_tx_pop_len = 0;
        // DO NOT re-arm RX here — see READ handler above.
        /* v0.4.6.1: check the TX arm return code; on failure record a
         * deferred diagnostic and re-arm RX immediately. */
        int32_t avail_rc = _spi_dev->spi_write_dma(_spi_tx_buf, 2, (void*)_spi_tx_callback);
        /* v0.4.20.38 (Phase 2): MISO-flag HIGH = response staged. */
        if (avail_rc == E_OK) {
            _spi_tx_staged = true;
            _spi_tx_staged_us = ma_get_time_us();
            hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
        }
        if (avail_rc != E_OK) {
            _spi_diag_record(2, avail_rc, 2);
            /* v0.4.20.56: reset _spi_tx_pending_len on failure, matching
             * READ/READ_CRC/TEST_PATTERN handlers. Prevents stale length
             * from corrupting the next command's TX-ring pop discipline. */
            _spi_tx_pending_len = 0;
            _spi_arm_rx();
        }
        break;
    }

    case FEATURE_TRANSPORT_CMD_RESET:
        _spi_rb_rx->clear();
        _spi_rb_tx->clear();
        _spi_tx_pending_len = 0;
        /* v0.4.14 (P1): a reset also disarms the bulk feeder (the S3 uses
         * RESET+START to restart a failed bulk transfer from offset 0). */
        _spi_bulk_armed = false;
        _spi_bulk_gen++;
        _spi_arm_rx();
        break;

    case FEATURE_TRANSPORT_CMD_START:
        /* v0.4.14 (P1): arm the bulk feeder. AT+SPIDUMP must have staged a
         * capture via ma::spi_bulk_prepare(); the feeder pushes it into
         * the TX ring from ma::spi_poll(). Re-arming restarts from
         * offset 0 (used by the S3's bulk retry).
         * v0.4.20.18: clear the TX ring first! Stale bytes (e.g. 0x05 from
         * a previous CMD_STOP response) linger in the ring; without a
         * clear, pre-fill pushes frame data AFTER the stale bytes and the
         * S3 reads garbage (CRC mismatch with data starting 05 05 05 05).
         * v0.4.20.56 (Claude F2): RE-ARM EARLY. The stale-LOW race: this
         * handler previously did ring-clear + 4KB prefill memcpy (~30-80us)
         * before re-arming, while the S3's next wait-LOW passed instantly
         * on the stale LOW and landed AVAILABLE mid-handler. Now: minimum
         * state change (ring clear), re-arm immediately, THEN slow work.
         * v0.4.20.56 (Claude F3): flag lifecycle consolidated to the
         * parse-retire block; the per-handler clears are deleted. */
        _spi_rb_tx->clear();
        _spi_arm_rx();
        /* --- Slow bookkeeping below (post-arm, not in race window) --- */
        _spi_bulk_offset = 0;
        _spi_bulk_gen++;
        _spi_bulk_armed = (_spi_bulk_buf != nullptr && _spi_bulk_len > 0);
        /* v0.4.20.23: arm the watchdog. */
        _spi_bulk_last_activity_us = ma_get_time_us();
        /* v0.4.20.10: pre-fill the TX ring NOW, in the command handler.
         * The S3 polls AVAILABLE immediately after CMD_START; if the
         * feeder hasn't run yet (it only runs in ma::spi_poll() at ~1kHz),
         * AVAILABLE returns 0 and the S3 tries to read 0 bytes, causing
         * a CRC protocol mismatch. Pre-filling ensures data is available.
         * (Post-arm per F2; AVAILABLE's zero-avail retry handles a short
         * first count.) */
        if (_spi_bulk_armed) {
            size_t remaining = _spi_bulk_len - _spi_bulk_offset;
            size_t pushed = _spi_rb_tx->push(
                (const char*)(_spi_bulk_buf + _spi_bulk_offset), remaining);
            _spi_bulk_offset += pushed;
            if (_spi_bulk_offset >= _spi_bulk_len) {
                _spi_bulk_armed = false;
            }
        }
        /* v0.4.20.56 (Claude F2): No late re-arm; already armed early above. */
        break;

    case FEATURE_TRANSPORT_CMD_STOP:
        /* v0.4.14 (P1): disarm the bulk feeder.
         * v0.4.20.56 (Claude F2): re-arm early — minimum state change,
         * then re-arm, no slow work in this handler.
         * v0.4.20.56 (Claude F3): flag lifecycle consolidated to the
         * parse-retire block; per-handler clears deleted. */
        _spi_bulk_armed = false;
        _spi_bulk_gen++;
        _spi_arm_rx();
        /* v0.4.20.58 (A0): end-of-bulk re-arm probe summary (post-arm,
         * not in the F2 race window). */
        if (_rearm_stats.responses || _rearm_stats.abandoned) {
            _rearm_stats_emit("stop");
        }
        break;

    case FEATURE_TRANSPORT_CMD_PARK:
        /* v0.4.20.54 (Claude PARK spec): Park the MISO flag for SD use.
         * No-response command. Re-arms RX but SKIPS the GPIO13 mux,
         * leaving PB2 released (tristate → S3 pull-up HIGH). The Himax
         * stays RX-armed and silent; SD can safely use SPI2. Exit is
         * implicit: the next command's normal handler restores GPIO13-LOW.
         * Disarm nothing, clear nothing — just park the flag. */
        _spi_parked = true;
        _spi_arm_rx_parked();
        break;

#if WATCHER_LINK_TEST_HOOKS
    case FEATURE_TRANSPORT_CMD_TEST_HOOK: {
        /* v0.4.20.62 (spec §5.6): injection trigger; len selects.
         * No-response command, like PARK. Compiled out of release. */
        if (len == 1) {
            /* Fail the next spi_open once (Guard 2/3 teardown tests). */
            _test_open_fails = 1;
            _spi_arm_rx();
        } else if (len == 2) {
            /* Park in the H-B unclaimed state: cycle to a fresh
             * controller, then deliberately do NOT arm and leave every
             * claim flag false, PB2 released (the wedge's signature).
             * H-B must fire within ~2 s and recover the link. */
            _spi_transport_cycle();
            _spi_rx_arm_failed = false;
            hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
        } else if (len == 3) {
            /* Stall the AT loop 60 s after the next real command. */
            _test_stall_pending = true;
            _test_stall_cmd_seen = false;
            _spi_arm_rx();
        } else {
            _spi_arm_rx();
        }
        break;
    }
#endif

    default:
        _spi_arm_rx();
        break;
    }
}

static void _spi_tx_callback(void* param) {
    (void)param;
    /* v0.4.12 (A-1): do NOT pop the TX ring here. This callback fires when
     * the TX DMA feeds the SSI FIFO — before the S3 clocks the bytes out —
     * so popping here loses bytes on a short response-read. The peeked
     * bytes are retired at the next valid command parse in
     * _spi_rx_callback (see above). */
    /* v0.4.20.7: do NOT re-arm RX here. The S3 hasn't clocked the TX data
     * yet; switching to RX mode now corrupts the transfer. Set _spi_tx_done;
     * ma::spi_poll() re-arms RX from thread context after the S3 finishes.
     * v0.4.20.20: record wall-clock timestamp and DMA length for
     * time-based re-arm (robust against yield-loop timing variations). */
    _spi_tx_done = true;
    _spi_tx_done_us = ma_get_time_us();
    _spi_tx_len_for_timing = (uint32_t)_spi_tx_pending_len;
}

SPI::SPI() : Transport(MA_TRANSPORT_SPI) {}

SPI::~SPI() { deInit(); }

ma_err_t SPI::init(const void* config) {
    if (_spi_opened || m_initialized) {
        return MA_OK;
    }

    (void)config;

    // Configure PB2-PB5 for SPI slave mode
    // PB2: MISO (DO), PB3: MOSI (DI), PB4: CLK, PB5: CS
    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_SPI_S_DO, 1);
    hx_drv_scu_set_PB3_pinmux(SCU_PB3_PINMUX_SPI_S_DI, 1);
    hx_drv_scu_set_PB4_pinmux(SCU_PB4_PINMUX_SPI_S_CLK, 1);
    hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI_S_CS, 1);

    /* v0.4.20.38 (Phase 2): MISO-flag handshake (replaces PB8).
     * PB2 swaps between SPI_S_DO and GPIO13 at runtime:
     * - GPIO13 driven LOW = RX armed, safe for S3 to send command
     * - SPI_S_DO (released, S3 pull-up) = HIGH = response staged, S3 can clock
     * The mux release IS the signal - atomic, race-free.
     * Set GPIO13 LOW before muxing to avoid glitches. */
    hx_drv_gpio_set_output(GPIO13, GPIO_OUT_LOW);
    hx_drv_scu_set_PB2_pinmux(SCU_PB2_PINMUX_GPIO13, 1);

    if (hx_drv_spi_slv_init(USE_DW_SPI_SLV_0, DW_SSPI_S_RELBASE) != 0) {
        return MA_EIO;
    }

    _spi_dev = hx_drv_spi_slv_get_dev(USE_DW_SPI_SLV_0);
    if (_spi_dev == nullptr) {
        return MA_EIO;
    }

    // SPI mode 0 (CPOL=0, CPHA=0) to match S3 master
    if (_spi_dev->spi_open(DEV_SLAVE_MODE, SPI_CPOL_0_CPHA_0) != 0) {
        return MA_EIO;
    }

    if (_spi_rx_buf == nullptr) {
        /* v0.4.12 (A-3): 32-byte aligned. hx_CleanDCache_by_Addr /
         * hx_InvalidateDCache_by_Addr operate on whole 32-byte cache
         * lines, so an 8-byte-aligned (pvPortMalloc) DMA buffer lets the
         * invalidate discard up to 24 bytes of adjacent heap metadata.
         * Aligned new now routes to the FreeRTOS heap (ma_device.cpp). */
        _spi_rx_buf = new (std::align_val_t{32}) uint8_t[SPI_PACKET_SIZE];
    }

    if (_spi_tx_buf == nullptr) {
        _spi_tx_buf = new (std::align_val_t{32}) uint8_t[4096];
    }

    if (_spi_rb_rx == nullptr) {
        _spi_rb_rx = new SPSCRingBuffer<char>(4096);
    }

    if (_spi_rb_tx == nullptr) {
        _spi_rb_tx = new SPSCRingBuffer<char>(4096);
    }

    if (!_spi_rx_buf || !_spi_tx_buf || !_spi_rb_rx || !_spi_rb_tx) {
        return MA_ENOMEM;
    }

    std::memset(_spi_rx_buf, 0, SPI_PACKET_SIZE);
    std::memset(_spi_tx_buf, 0, 4096);

    _spi_opened = m_initialized = true;

    /* v0.4.20.52 (Claude Q1): Take the register base from the driver via
     * SPI_CMD_GET_SPI_BASE_REG once at init, cache it, and cross-check
     * against DW_SSPI_S_RELBASE. The ioctl is the single source of truth;
     * if the driver ever translates addresses, we want to know loudly.
     * Expected 0x50810000 (BASE_ADDR_AHB_SPI_S_ALIAS, secure view). */
    {
        uint32_t drv_base = 0;
        int32_t rc = _spi_dev->spi_control(SPI_CMD_GET_SPI_BASE_REG,
                                           (void *)&drv_base);
        _spi_ssi_base_cached = drv_base;
        MA_LOGE(MA_TAG, "SPI base: drv=0x%08lx rc=%ld (expect 0x50810000)",
                (unsigned long)drv_base, (long)rc);
        if (drv_base != (uint32_t)DW_SSPI_S_RELBASE) {
            MA_LOGE(MA_TAG, "SPI base MISMATCH: drv=0x%08lx vs RELBASE=0x%08lx",
                    (unsigned long)drv_base,
                    (unsigned long)(uint32_t)DW_SSPI_S_RELBASE);
        }
    }

    _spi_arm_rx();

    return MA_OK;
}

void SPI::deInit() {
    if (!_spi_opened || !m_initialized) {
        return;
    }

    if (_spi_dev) {
        _spi_dev->spi_close();
        _spi_dev = nullptr;
    }

    if (_spi_rb_rx) {
        delete _spi_rb_rx;
        _spi_rb_rx = nullptr;
    }

    if (_spi_rb_tx) {
        delete _spi_rb_tx;
        _spi_rb_tx = nullptr;
    }

    if (_spi_rx_buf) {
        /* v0.4.12 (A-3): allocated with aligned new[] above — must pair
         * with the aligned delete (the raw pointer is stashed just before
         * the aligned block, not at it). */
        ::operator delete[](_spi_rx_buf, std::align_val_t{32});
        _spi_rx_buf = nullptr;
    }

    if (_spi_tx_buf) {
        ::operator delete[](_spi_tx_buf, std::align_val_t{32});
        _spi_tx_buf = nullptr;
    }

    _spi_opened = m_initialized = false;
    _spi_rx_armed = false;  /* v0.4.20.62 (WI-5 H-B tracker hygiene) */
}

size_t SPI::available() const { return _spi_rb_rx ? _spi_rb_rx->size() : 0; }

size_t SPI::send(const char* data, size_t length) {
    if (!m_initialized || data == nullptr || length == 0 || !_spi_rb_tx) {
        return 0;
    }

    Guard guard(_spi_tx_mutex);

    return _spi_rb_tx->push(data, length);
}

size_t SPI::flush() {
    if (!m_initialized) {
        return (size_t)-1;
    }

    return 0;
}

size_t SPI::receive(char* data, size_t length) {
    if (!m_initialized || length == 0 || !_spi_rb_rx) {
        return 0;
    }

    return _spi_rb_rx->pop(data, length);
}

size_t SPI::receiveIf(char* data, size_t length, char delimiter) {
    if (!m_initialized || length == 0 || !_spi_rb_rx) {
        return 0;
    }

    return _spi_rb_rx->popIf(data, length, delimiter);
}

/* v0.4.6.1: thread-context SPI housekeeping.
 * Call periodically from the AT-server main loop (NOT from ISR/callback).
 *  - Prints any deferred DMA-failure diagnostics recorded by the callbacks.
 *  - Retries a failed RX DMA arm (Fix 3: the callback-side retry was dead
 *    code — a failed arm leaves RX disarmed so no callback can fire). */
/* v0.4.14 (P1): stage a capture buffer for the bulk feeder. Called by the
 * AT+SPIDUMP handler after capturing a frame. The buffer must stay valid
 * until the S3 finishes draining it (CMD_STOP / next SPIDUMP). */
void spi_bulk_prepare(const uint8_t* buf, size_t len) {
    _spi_bulk_armed  = false;
    _spi_bulk_gen++;
    _spi_bulk_buf    = buf;
    _spi_bulk_len    = len;
    _spi_bulk_offset = 0;
}

/* v0.4.20.62 (WI-4/WI-5): the one spi_open attempt path — shared by
 * the teardown cycle below and Guard 3's block-head retry. Applies
 * the (compile-time-off) open-failure injection when built in. */
static bool _spi_try_open() {
    if (!_spi_dev) {
        return false;
    }
#if WATCHER_LINK_TEST_HOOKS
    if (_test_open_fails > 0) {
        _test_open_fails--;
        return false;
    }
#endif
    if (_spi_dev->spi_open(DEV_SLAVE_MODE, SPI_CPOL_0_CPHA_0) == 0) {
        _spi_opened = true;
    }
    return _spi_opened;
}

/* v0.4.20.62 (WI-4/WI-5): the one teardown path, shared by Guard 2,
 * Guard 3 and the H-B watchdog (spec §6.1 asks for the factoring):
 * cycle SSIENR via spi_close/spi_open, which resets controller + DMA
 * state. Returns true iff the transport is open afterwards. Flag
 * policy stays with the callers: Guard 3 (H-A(2)) keeps
 * _spi_rx_arm_failed set when this returns false and skips its arm;
 * Guard 2 / H-B arm afterwards and let _spi_arm_rx()'s H-A(1)
 * behaviour report a still-closed transport into Guard 3. */
static bool _spi_transport_cycle() {
    if (!_spi_dev) {
        return false;
    }
    _spi_rx_armed = false;  /* the close kills any outstanding RX DMA */
    if (_spi_opened) {
        _spi_dev->spi_close();
        _spi_opened = false;
    }
    return _spi_try_open();
}

/* v0.4.20.62 (WI-4 H-C): verify the arm took. The arm's DMA half is
 * proven by spi_read_dma's return code; its flag-line half — PB2
 * muxed to GPIO13 and driven LOW — was previously taken on faith, and
 * a lost mux write leaves the S3 facing a permanently HIGH line with
 * every Himax flag reading healthy. Read both halves back through
 * the driver (post-arm, so the S3-visible re-arm gains no latency;
 * invariant 2 permits post-arm readbacks). Only a POSITIVE mismatch
 * fails the arm — a readback error abstains, in the same spirit as
 * the re-arm probes' dead-probe abstention — and failure is reported
 * the Guard-3 way: _spi_rx_arm_failed. */
static void _spi_verify_arm_took() {
    SCU_PINMUX_CFG_T cfg;
    if (hx_drv_scu_get_all_pinmux_cfg(&cfg) != SCU_NO_ERROR) {
        return;
    }
    if (cfg.pin_pb2 != SCU_PB2_PINMUX_GPIO13) {
        _spi_rx_arm_failed = true;
        return;
    }
    uint8_t level = 1;
    if (hx_drv_gpio_get_in_value(GPIO13, &level) != GPIO_NO_ERROR) {
        return;
    }
    if (level != GPIO_OUT_LOW) {
        _spi_rx_arm_failed = true;
    }
}

/* v0.4.20.62 (WI-5): spi_poll instrumentation/watchdog state. */
static int64_t _poll_last_call_us    = 0;
static int64_t _poll_max_gap_us      = 0;
static int64_t _spi_unclaimed_since_us = 0;

void spi_poll() {
    /* v0.4.20.62 (WI-5 H-D): poll-gap instrumentation — the H1/H2
     * discriminator. Runs BEFORE the closed-transport early return on
     * purpose: in the H1 end-state the AT loop keeps calling spi_poll
     * (gaps stay ~1 ms — loop alive, state uncovered), while an H2
     * AT-thread stall appears as one gap ≈ the stall duration when
     * the loop resumes. Gaps are only measured while the transport is
     * supposed to be live (open, or closed with an arm failure pending
     * recovery), so an intentional deInit accrues no spurious gap. */
    {
        int64_t now_us = ma_get_time_us();
        bool live = _spi_opened || _spi_rx_arm_failed;
        if (live && _poll_last_call_us != 0) {
            int64_t gap_us = now_us - _poll_last_call_us;
            if (gap_us > _poll_max_gap_us) {
                _poll_max_gap_us = gap_us;
            }
            if (gap_us > 500000) {
                diag_nb_emit("E DIAG POLL_GAP %ld", (long)(gap_us / 1000));
            }
        }
        _poll_last_call_us = live ? now_us : 0;
    }
#if WATCHER_LINK_TEST_HOOKS
    /* Injection selector 3: stall the AT loop once, for 60 s, after a
     * real command was parsed. The busy-wait is the point — the poll,
     * and with it the AT loop, must stop calling in (the H2 shape). */
    if (_test_stall_pending && _test_stall_cmd_seen) {
        _test_stall_pending  = false;
        _test_stall_cmd_seen = false;
        int64_t stall_end = ma_get_time_us() + 60000000;
        while (ma_get_time_us() < stall_end) {
        }
    }
#endif
    /* v0.4.20.62 (WI-4 H-A(2)): the early return must not fire while
     * an arm failure is pending — with _spi_opened false the Guard-3
     * block below is the only thing that can retry the open, and the
     * pre-.62 head check made it unreachable, sealing the wedge. */
    if (!_spi_opened && !_spi_rx_arm_failed) {
        return;
    }
    if (_spi_diag.pending) {
        uint8_t  where = _spi_diag.where;
        int32_t  rc    = _spi_diag.rc;
        uint32_t len   = _spi_diag.len;
        _spi_diag.pending = false;
        const char* what =
            (where == 1) ? "spi_write_dma (READ)" :
            (where == 2) ? "spi_write_dma (AVAILABLE)" :
            (where == 9) ? "parser-drop (feature!=0x10)" :
                           "spi_read_dma";
        if (where == 9) {
            /* Parser drop: rc=buf[0], len=buf[2] */
            diag_nb_emit("E DIAG parser-drop: buf[0]=0x%02x buf[2]=0x%02x", (unsigned)rc & 0xFF, (unsigned)len & 0xFF);
        } else if (where == 10) {
            /* Parser realign (v0.4.20.60): rc=offset, len=cmd */
            diag_nb_emit("I DIAG parser-realign: off=%ld cmd=0x%02lx", (long)rc, (unsigned long)len & 0xFF);
        } else {
            MA_LOGE(MA_TAG, "DIAG %s arm failed: rc=%ld len=%lu", what, (long)rc, (unsigned long)len);
        }
    }
    /* v0.4.20.13: deferred CMD_READ_CRC diagnostic. Shows what the S3
     * requested vs what was actually staged, for debugging CRC mismatches. */
    if (_spi_readcrc_diag.pending) {
        uint16_t req = _spi_readcrc_diag.req_len;
        uint32_t av = _spi_readcrc_diag.avail;
        uint16_t st = _spi_readcrc_diag.staged_len;
        uint16_t c = _spi_readcrc_diag.crc;
        _spi_readcrc_diag.pending = false;
        /* v0.4.20.55: Non-blocking DIAG. Format to ring, drain when idle. */
        diag_nb_emit("E DIAG READ_CRC req=%u avail=%lu staged=%u crc=%04x",
                req, (unsigned long)av, st, c);
        if (st >= 4) {
            diag_nb_emit("E DIAG READ_CRC data: %02x %02x %02x %02x ... %02x %02x %02x %02x",
                    _spi_readcrc_diag.first4[0], _spi_readcrc_diag.first4[1],
                    _spi_readcrc_diag.first4[2], _spi_readcrc_diag.first4[3],
                    _spi_readcrc_diag.last4[0], _spi_readcrc_diag.last4[1],
                    _spi_readcrc_diag.last4[2], _spi_readcrc_diag.last4[3]);
        }
    }
    /* v0.4.20.55: Drain non-blocking DIAG ring when SPI is idle. */
    diag_nb_drain();
    if (_spi_rx_arm_failed) {
        _spi_rx_arm_failed = false;
        /* v0.4.20.62 (WI-4 H-A(2)): if a previous teardown's spi_open
         * failed, the transport is closed and the arm below cannot
         * succeed — retry the open FIRST (at most one open attempt per
         * Guard-3 entry; this block is the escalation cycle). */
        if (!_spi_opened && _spi_dev) {
            _spi_try_open();
        }
        if (_spi_opened) {
            /* v0.4.20.56 (Claude F4): Park-aware retry. If parked, re-arm via
             * the parked path to avoid driving LOW mid-SD-transaction. */
            if (_spi_parked) {
                _spi_arm_rx_parked();
            } else {
                _spi_arm_rx();
            }
        } else {
            /* Open still failing: stay in Guard 3 (counted below); the
             * next entry retries open+arm on the existing cadence. */
            _spi_rx_arm_failed = true;
        }
        /* v0.4.20.52 (Claude Q4 Guard 3): arm-failure escalation. If the
         * retry also failed, count it. After ~10 consecutive failures,
         * teardown (spi_close/spi_open) and re-arm. Counter, not wall clock.
         * This is the sanctioned recovery for persistent _spi_arm_rx()
         * failure: keep retrying from thread context, never drive LOW while
         * unarmed, escalate to teardown on a counter. */
        if (_spi_rx_arm_failed) {
            _spi_rx_arm_fail_count++;
            if (_spi_rx_arm_fail_count >= 10) {
                _spi_rx_arm_fail_count = 0;
                _spi_wd_aborts++;
                _spi_tx_staged = false;
                _spi_tx_staged_us = 0;
                _spi_tx_done = false;
                _spi_tx_done_us = 0;
                _rearm_probe_drop();
                if (_spi_transport_cycle()) {
                    _spi_rx_arm_failed = false;
                    /* v0.4.20.56 (Claude F4): Park-aware escalation re-arm. */
                    if (_spi_parked) {
                        _spi_arm_rx_parked();
                    } else {
                        _spi_arm_rx();
                    }
                }
                /* v0.4.20.62 (WI-4 H-A(2)): if the cycle's open failed,
                 * _spi_rx_arm_failed STAYS true and the arm is skipped.
                 * The pre-.62 code cleared the flag unconditionally at
                 * this point, which — combined with the then-silent
                 * _spi_arm_rx — manufactured the incident's
                 * unrecoverable end-state: all claim flags false, RX
                 * unarmed, transport closed, no guard able to fire. */
            }
        } else {
            _spi_rx_arm_fail_count = 0;
        }
    }
    /* v0.4.20.7: deferred RX re-arm after TX. The TX callback sets
     * _spi_tx_done when the DMA feeds the FIFO; we re-arm RX here (thread
     * context) after the S3 has finished clocking the TX bytes.
     * v0.4.20.20: wall-clock based. Required wait = (len*8/12MHz) + 2ms
     * margin. Robust against AT-server yield-loop timing variations
     * (the v0.4.20.17/19 poll-countdown assumed a 20us period that
     * doesn't exist).
     * v0.4.20.21: margin 500us (was 2000us). The 2ms margin made the
     * Himax re-arm too slowly; the S3 sends the next CMD_READ_CRC
     * before RX is re-armed, losing the command. 500us covers S3
     * driver overhead while keeping up with back-to-back chunks.
     * v0.4.20.22: margin 100us. Post-failure analysis shows the S3
     * sends CMD_RESET immediately after a CRC failure; if the Himax
     * hasn't re-armed RX yet, the reset is lost and the two sides
     * desync (S3's AT rx buffer floods with ~20 "rx buffer is full").
     * 100us is enough for the DW FIFO to drain at 12MHz.
     * v0.4.20.24: margin 6000us — WRONG! This broke spitest. The
     * 6ms wait means the Himax re-arms too slowly; the S3's next
     * command (sent immediately after the previous response) is
     * lost. The 5ms S3 delay is specific to the bulk AVAILABLE path,
     * not the TEST_PATTERN path. Reverting to 500us.
     * v0.4.20.25: margin 500us (revert of 0.4.20.24). The proper fix
     * requires CS-edge detection or S3-side flow control, not a
     * fixed timeout. 500us is the best compromise: fast enough for
     * back-to-back commands, slow enough for the DW FIFO to drain.
     * v0.4.20.26: per-response-size margin! The 500us margin works
     * for TEST_PATTERN (S3 clocks immediately) but fails for
     * CMD_AVAILABLE (S3 waits 5ms in spi_bulk_poll_available_()
     * before clocking the 2-byte response). The Himax timer
     * (transfer_time + 500us) expires at 0.5ms, re-arms RX, then the
     * S3 clocks at 5ms but Himax is in RX mode → garbage. The S3
     * then reads stale AVAILABLE bytes (03 03...) instead of frame
     * data → CRC fail. For tiny responses (<=2 bytes, i.e.
     * AVAILABLE), use 6000us margin to cover the S3's 5ms delay.
     * For bulk data, keep 500us (S3 clocks promptly after READ_CRC).
     * v0.4.20.27: 500us ALSO fails for TEST_PATTERN (0/16)! The S3
     * has pre-read delays on ALL paths, not just AVAILABLE. The
     * 2000us margin from 0.4.20.20 had 16/16 passing. Reverting to
     * 2000us for all response sizes. The per-size optimization was
     * wrong — the S3's delay is not limited to AVAILABLE.
     * v0.4.20.28: FLOW CONTROL (no more guessing!). S3-paced protocol:
     * The S3 (0.4.18.18+) guarantees a 1ms inter-command gap after
     * receiving a response before sending the next command. The S3
     * also clocks responses immediately (no pre-read delay). The
     * Himax timer is now (transfer_time + 1500us): 1000us for the
     * S3's guaranteed gap + 500us safety margin. This is robust
     * because the 1ms gap is a protocol guarantee from the S3, not
     * a guess about S3 timing. The pre-read delays are removed from
     * the S3 side. */
    /* v0.4.20.34: Adaptive timeout. The S3's 2ms pre-clock (0.4.18.28) eats
     * into the timeout budget. For large transfers (bulk, len~4090), the
     * Himax must not timeout mid-transfer: timeout = transfer_time + 2000us
     * (pre-clock) + 500us (margin). For small transfers (spitest, len<=64),
     * the S3 sends next command quickly; use 1500us to re-arm in time.
     * Formula: timeout = transfer_time + (len > 64 ? 5000 : 1500).
     * - len=4090: 2726+5000=7726us. S3 finish 4600us (from DMA), next 6600us. OK.
     * - len=16: 10+1500=1510us. S3 next 2010us. OK.
     * v0.4.20.36: Bumped large-transfer margin 2500->5000us. Experiment to
     * test if RTOS jitter was causing premature timeout (05s pattern).
     * This is a workaround; Phase 2 GPIO READY is the proper fix. */
/* v0.4.20.41 (Phase A1): Status-based TX→RX re-arm. Replaces the wall-clock
 * guess (transfer_time + 1500/5000us) with hardware truth.
 *
 * The old timer raced the S3's clocking: if the S3 was slow, the Himax re-armed
 * early (S3 read zeros from GPIO); if the S3 clocked late, the bytes were
 * injected into the freshly-armed RX as junk, desyncing the next command.
 * Ten margin revisions (v0.4.20.17→.36) proved no constant can win.
 *
 * v0.4.20.43: REVERTED the direct SSI register reads (TXFLR/SR). They caused
 * a bus fault during bulk transfers (Himax reboot at 21:40:51). The ALIAS
 * address didn't help — the peripheral is likely secure-only or the
 * context doesn't permit it. Using a 1ms timer instead: S3 v0.4.18.51+
 * clocks immediately after wait-HIGH (no 2ms delay), so 1ms is safe.
 * The 300ms watchdog remains for abandoned responses. */

    /* v0.4.20.52 (Claude Q4): Three guards, none inferring S3 state from time.
     *
     * Guard 1 — Drained path: _spi_tx_done set by DMA-completion callback.
     * 25ms interim timer, then re-arm. Clears _spi_tx_staged.
     * (v0.4.20.59: Guard 1 is signal-driven — drained signals + a 2ms
     * floor; the 25/30ms values survive only as the fallback ceiling.)
     *
     * Guard 2 — Staged watchdog (300ms), gated on _spi_tx_staged, NOT on
     * _spi_tx_done. This is the fix for the hole: for responses larger than
     * the 32-byte FIFO, demand-paced DMA only completes once the master has
     * clocked most of it. If the S3 abandons a 4092-byte response (never
     * clocks), the callback never fires, _spi_tx_done stays false, and a
     * done-gated watchdog never arms — for exactly the abandoned-response
     * case it exists for. Gating on _spi_tx_staged (set at spi_write_dma OK)
     * covers abandoned responses of any size.
     *
     * Guard 3 — Arm-failure escalation (below, in the _spi_rx_arm_failed
     * retry block): after ~10 consecutive _spi_arm_rx() failures, teardown.
     * Counter, not wall clock.
     *
     * The v0.4.20.50 100ms stuck-HIGH watchdog is retired — subsumed by
     * guards 2 and 3. Guard 2 catches abandoned responses correctly (without
     * the 100ms tear-down-a-slow-reader hazard); guard 3 catches the
     * failed-arm state. */
    if (_spi_tx_done) {
        /* v0.4.20.58 (A0): sample the TX-drain probes while the re-arm
         * is pending. v0.4.20.59: the samples now DRIVE the re-arm. */
        _rearm_probe_sample();
        uint32_t elapsed = (uint32_t)(ma_get_time_us() - _spi_tx_done_us);
        uint32_t timeout_us = (_spi_tx_len_for_timing > 64) ? 30000 : 25000;
        /* v0.4.20.59 (A0): signal-driven re-arm. Arm at the first poll
         * where the drain signals agree AND the S3's ~2ms minimum
         * inter-command gap has elapsed since DMA-done (the gap covers
         * the ≤33-frame FIFO-residue clock-out and the master's
         * turnaround). The 25/30ms timer is demoted to a counted
         * fallback ceiling. Evidence: prep doc Phase 3/4 (.58: signals
         * clear in 0-1ms; per-signal not-clear ~0 in 11,408 responses). */
        bool drained = _rearm_signals_drained();
        bool signal_arm = drained && elapsed >= 2000;
        if (signal_arm || elapsed > timeout_us) {
            _spi_tx_done = false;
            _spi_tx_done_us = 0;
            _spi_tx_staged = false;
            _spi_tx_staged_us = 0;
            _spi_arm_rx();
            /* v0.4.20.62 (WI-4 H-C): verify the arm took (PB2 mux +
             * GPIO13 level readback). Skipped when the arm already
             * reported a DMA failure — nothing to verify, Guard 3
             * owns that state. A mismatch is handed to Guard 3 via
             * _spi_rx_arm_failed and acted on at the next poll. */
            if (!_spi_rx_arm_failed) {
                _spi_verify_arm_took();
            }
            _rearm_probe_finish(signal_arm, (int32_t)elapsed);
            /* Learn the TXAVAIL capacity (post-re-arm the TX FIFO is
             * empty by construction). */
            if (!_probe_dead_txavail && _spi_dev) {
                int32_t v = -1;
                if (_spi_dev->spi_control(SPI_CMD_GET_TXAVAIL, (void*)&v) == E_OK &&
                    v > _probe_txavail_capacity) {
                    _probe_txavail_capacity = v;
                }
            }
        }
    }
    /* Guard 2: staged watchdog. Independent of Guard 1. */
    if (_spi_tx_staged) {
        uint32_t elapsed_staged = (uint32_t)(ma_get_time_us() - _spi_tx_staged_us);
        if (elapsed_staged > 300000) {
            /* 300ms staged watchdog (Claude Q4, MFH/1 W1 rev C): S3 abandoned
             * the staged response (never clocked it). Covers any size,
             * including large responses whose DMA never completed.
             * CRITICAL: teardown before re-arm. spi_close/spi_open cycles
             * SSIENR and resets controller + DMA state. */
            _spi_wd_aborts++;
            _spi_tx_staged = false;
            _spi_tx_staged_us = 0;
            _spi_tx_done = false;
            _spi_tx_done_us = 0;
            _rearm_probe_drop();
            /* v0.4.20.62 (WI-5 factoring): the shared teardown path.
             * If the re-open fails, the arm below now reports the
             * closed transport into Guard 3 via H-A(1) instead of
             * silently no-oping (the pre-.62 behaviour). */
            _spi_transport_cycle();
            _spi_arm_rx();
        }
    }
    /* v0.4.20.62 (WI-5 H-B): flag-independent stuck-unarmed watchdog.
     * Guards 1-3 each key on a flag (done / staged / arm_failed); the
     * incident state had all three false with RX unarmed, so nothing
     * could ever fire. H-B keys on the armed tracker instead: the
     * transport is open, no RX DMA is outstanding, and NO known
     * in-flight state claims the link (no staged TX, no TX awaiting
     * Guard 1, no arm-failure escalation in progress, no bulk feeder),
     * continuously for >2 s — then force the shared teardown + re-arm.
     * 2 s is ~6x Guard 2's 300 ms worst legitimate unarmed window;
     * every known state exempts itself, so on a healthy link this
     * never fires. (_spi_parked needs no explicit exemption: a parked
     * arm is a real arm, so _spi_rx_armed is true while parked; the
     * recovery below is still park-aware out of caution.) */
    if (_spi_opened && !_spi_rx_armed && !_spi_tx_staged &&
        !_spi_tx_done && !_spi_rx_arm_failed && !_spi_bulk_armed) {
        int64_t now_us = ma_get_time_us();
        if (_spi_unclaimed_since_us == 0) {
            _spi_unclaimed_since_us = now_us;
        } else if (now_us - _spi_unclaimed_since_us > 2000000) {
            _spi_unclaimed_since_us = 0;
            _spi_wd_aborts++;
            diag_nb_emit("E DIAG STUCK_UNARMED teardown");
            _spi_transport_cycle();
            if (_spi_parked) {
                _spi_arm_rx_parked();
            } else {
                _spi_arm_rx();
            }
        }
    } else {
        _spi_unclaimed_since_us = 0;
    }

    /* v0.4.14 (P1): bulk feeder. While armed (S3 sent CMD_START after
     * AT+SPIDUMP), push the staged capture buffer into the TX ring for the
     * S3 to drain with CMD_READ_CRC. push() is non-blocking; a full ring
     * simply retries on the next poll (~1 kHz). Disarms when every byte
     * is queued — the S3 drains the remainder from the ring.
     * v0.4.20.23: watchdog — if no READ_CRC within 1s of arming/last
     * read, disarm. Handles lost CMD_RESET (S3 sent it while Himax
     * hadn't re-armed RX yet). Prevents the feeder from pushing frame
     * data while the S3 is back in AT mode (which floods the S3's AT
     * parser → "rx buffer is full" x20). */
    if (_spi_bulk_armed) {
        int64_t now_us = ma_get_time_us();
        if ((now_us - _spi_bulk_last_activity_us) > 1000000) {
            _spi_bulk_armed = false;
            _spi_bulk_gen++;
            /* v0.4.20.58 (A0): bulk ended via the feeder watchdog, not
             * CMD_STOP — still emit the re-arm probe summary. */
            if (_rearm_stats.responses || _rearm_stats.abandoned) {
                _rearm_stats_emit("watchdog");
            }
        }
    }
    if (_spi_bulk_armed && _spi_bulk_buf != nullptr &&
        _spi_bulk_offset < _spi_bulk_len) {
        size_t remaining = _spi_bulk_len - _spi_bulk_offset;
        size_t pushed = _spi_rb_tx->push(
            (const char*)(_spi_bulk_buf + _spi_bulk_offset), remaining);
        _spi_bulk_offset += pushed;
        if (_spi_bulk_offset >= _spi_bulk_len) {
            _spi_bulk_armed = false;
        }
    }
}

}  // namespace ma
