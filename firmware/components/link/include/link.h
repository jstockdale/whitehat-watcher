#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the JSON line-protocol link on the USB-serial console (stdin/stdout).
 *  Must be called after hid_ble/wifi/eyes are initialized. */
esp_err_t link_init(void);

/** Firmware version string reported by {"t":"dbg","cmd":"version"}. */
void link_set_fw_version(const char *v);

/** Send a JSON object to the host (one line, newline-terminated).
 *  Takes ownership of `obj` and frees it. Thread-safe.
 *  Returns ESP_OK on success; logs loudly and returns an error if the
 *  object is NULL, serialization fails, or the TX lock is missing.
 *  (v0.2.0 dropped the boot "ready" event silently here on a transient
 *  heap squeeze -- never fail quietly again.) */
esp_err_t link_send(cJSON *obj);

/** v0.4.18.81: Ring fill level (0-100%) for PNG chunk flow control.
 * 100 = full or unavailable (conservative). */
int link_ring_fill_pct(void);

/** Block until the ring has required_bytes free PLUS headroom.
 * (v0.4.21: the headroom keeps post-push backlog off 100% so stray log
 * writes can't lap the TX reader into chunk data — see
 * docs/chunk-loss-rca-2026-11-02.md.)
 * Returns true when space is available, false on timeout_ms expiry.
 * Watchdog-friendly (50ms delays). */
bool link_wait_for_ring_space(uint32_t timeout_ms, size_t required_bytes);

/** v0.4.35: ack'd chunk streaming (John's design). When a snaptest is
 * dispatched with "ack":1, the chunk sender (main.c) emits ONE chunk
 * and waits for the host's {"t":"chunk_ack","id":N,"seq":S} before
 * advancing — loss becomes backpressure instead of holes in the image.
 * A {"t":"chunk_nack"} for the awaited seq (host received the chunk
 * but it failed CRC/length validation) or a timeout resends the same
 * chunk; the sender bounds the attempts and then aborts the stream.
 * Ack/nack lines are consumed in the rx parser, never queued. */
/** The dbg png stream is singleton and synchronous and its command
 * carries no host-chosen id, so its chunks ack under this reserved
 * id (host-chosen snaptest ids are >= 0 and can never collide). */
#define LINK_PNG_STREAM_ID (-2)
void link_chunk_ack_arm(int id);
void link_chunk_ack_disarm(void);
bool link_chunk_ack_active(void);
/** Snapshot the nack counter before (re)sending a chunk, so
 * link_chunk_ack_wait() can tell a nack for THIS send from a stale
 * one. Call immediately before each chunk send in ack mode. */
void link_chunk_ack_note_send(void);
/** Wait for the ack of (id, seq). The timeout is baud-aware: the
 * chunk's own wire time at the current rate plus 600 ms of slack,
 * floored at 1000 ms (a flat 1 s would resend chunks still draining
 * at 115200 and duplicate-spiral; at 460800/750000 the floor IS the
 * ~1 s budget). True when the host acked seq (or a later one —
 * cumulative). False on timeout, on a nack for seq, or when
 * disarmed mid-wait (gate lifts safely). */
bool link_chunk_ack_wait(int id, int seq, size_t chunk_chars);

/** Convenience: {"t":"ok","cmd":"<cmd>"} (+ "_mid" when set).
 *  Returns link_send()'s result; callers may ignore it. */
esp_err_t link_respond_ok(const char *cmd);

/** Convenience: {"t":"err","cmd":"<cmd>","msg":"<msg>"} (+ "_mid" when set).
 *  Returns link_send()'s result; callers may ignore it. */
esp_err_t link_respond_err(const char *cmd, const char *msg);

/** Convenience: {"t":"<type>"} — fill in more fields before link_send. */
cJSON *link_new_event(const char *type);

/** Set the purple "AI thinking" LED. Auto-clears after 60s if the host
 *  doesn't clear it first (safety against a lost "ai off" command).
 *  The host should send {"t":"dbg","cmd":"ai","on":0} when AI processing
 *  finishes. Thread-safe. */
void link_ai_thinking_start(void);

/** Clear the "AI thinking" LED and cancel the auto-clear timer.
 *  Thread-safe. */
void link_ai_thinking_stop(void);

/** Async-command lifecycle: clear the pending-registry entry for `mid`
 *  when the async completion event has been emitted.
 *  Returns true if the completion event should be emitted (entry found
 *  and cleared, or nothing was tracked for this mid). Returns false if
 *  the entry was already force-completed by the timeout monitor — the
 *  caller must then DROP the late completion to preserve exactly-once
 *  terminal replies. Thread-safe; called from completion callbacks. */
bool link_pending_done(const char *mid);

/** Parse a JSON command line and enqueue it on the worker path, exactly
 *  as if it arrived over the USB-serial console. Used for verified
 *  native-MQTT commands. The command is tagged _via_mqtt (serial-only
 *  provisioning commands are refused for such commands). Thread-safe;
 *  drops with a correlated "busy" reply when the worker queue is full. */
void link_sealed_dispatch(const char *json, size_t len);

/** Register a callback invoked with the TX lock held for every serial
 *  TX line, before the line buffer is freed. The callback must be
 *  non-blocking and must never call link_send (no recursion). Used by
 *  the native-MQTT component to mirror TX lines to the broker. */
void link_set_tx_mirror(void (*cb)(const char *line, size_t len));

#ifdef __cplusplus
}
#endif
