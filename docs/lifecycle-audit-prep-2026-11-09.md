# Lifecycle audit prep (2026-11-09) — the re-arm bug's pattern, hunted

Context: suggestion #4 from the 2026-11-09 architecture review. The
0.4.45 wake-word re-arm bug was a hand-rolled teardown (the detection
path) bypassing the single owner function (`ww_tasks_stop`) and
leaving `ww_feed_task_` as a stale handle to a dead task; the
restart guard then concluded "already running" forever. This prep
scans the firmware for the same shape so the audit discussion starts
from evidence. Read-only; nothing changed.

## Scan

Task create/delete sites concentrate in: wakeword.c (12),
sscma_client_ops.c (12), eyes.c (11), seal_dev.c (9), link.c (7),
ptt.c (3), input.c (3).

## Findings

1. **input.c `retry_task_` — SOUND (the correct pattern).** The
   retry supervisor nulls its own handle *before* self-deleting
   (`retry_task_ = NULL; vTaskDelete(NULL);`), so the
   already-running guard in `input_start_retry_task()` can never
   see a stale handle. Residual nits, both accepted-by-design: a
   microscopic race if a new supervisor starts between the NULL and
   the delete, and the task_psram wrapper leak the helper's own
   header documents for self-deleting tasks.
2. **sscma_client process/monitor tasks — SOUND.** Created per
   client instance (static/task variants), deleted by handle at
   deinit by the owner. No per-operation hand-rolled teardown —
   structurally unlike the re-arm bug.
3. **wakeword.c:360 — the known fix site** (0.4.45's `ww_reap`),
   present as expected. The 0.4.49 ship task was designed against
   this lesson: created once at init, never per take.
4. **eyes.c workers — SOUND, and the codebase's best pattern.**
   Guards are bool flags, not handles: `relink_inflight_` is
   cleared by the relink worker itself on every exit path (and by
   the starter if creation fails); `snap_busy_` claims the single
   snapshot slot and carries a 60 s steal timeout so even a wedged
   holder self-heals. Because the guard is a flag the worker owns,
   no path can strand it the way `ww_feed_task_` was stranded.
   The snap/snaptest transient tasks use generation-checked handle
   release with deliberate PSRAM-stack leaks on self-delete
   (documented in-tree: "leak is safer than crash").
5. **seal_dev.c — SOUND.** One persistent worker task created once
   at init with a static (DRAM) stack via xTaskCreateStatic;
   requests arrive by queue. Its only vTaskDelete is an init-time
   sanity abort if the stack lands in the wrong memory class.

## Conclusion

No second instance of the re-arm shape found. Every task
lifecycle in the tree uses one of three sound patterns:
owner-reaped handles (sscma), self-clearing handles (input), or
bool flags with timeouts (eyes) — plus create-once persistent
workers (seal_dev, wakeword post-fix, ptt_ship by design).

## For the audit discussion

- Consider codifying the standing rule the codebase already
  follows de facto: any task that can exit must be owner-reaped,
  clear its own handle/flag before dying, or never exit — and
  guards key on flags, never on bare task handles.
