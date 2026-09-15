// SoundForge G2 P5 — lock-free SPSC command queue (§4.4, §4.5).
// Transports *intent*: commands are applied via the G2 sf_graph_apply_batch
// loop (internal apply_batch_impl — SEC-G3-1). Since G3 P4a the queue's live
// consumer is sf_queue_runner (C5/C6 rewritten); sync sf_graph_apply_batch
// stays available but busy-rejects while a runner is active (C7).
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "soundforge/sf_types.h"
#include "soundforge/sf_project.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Command types carried by the queue (D3-amd / D4; slot size stays 176 B —
   the "revisit with the G3 command set" note is retired with this decision).
   0 stop       — runner-only CONTROL command (ORC-2 STOP barrier) and
                  0/7/8 are REJECTED by sync sf_graph_apply_batch.
   7 evaluateMixer — runner-only query: the runner stores the result JSON as
                  its bounded last_report; never applied as a mutation.
   8 setOutput  — runner-only CONTROL command (G5 P1): the runner forwards
                  id1 to the engine observer, which recompiles/publishes the
                  plan with the new target; never applied as a mutation. */
#define SF_CMD_STOP          0
#define SF_CMD_ADD_NODE       1
#define SF_CMD_REMOVE_NODE    2
#define SF_CMD_ADD_EDGE       3
#define SF_CMD_REMOVE_EDGE    4
#define SF_CMD_SET_MIXER      5
#define SF_CMD_SET_PRESET     6
#define SF_CMD_EVALUATE_MIXER 7
#define SF_CMD_SET_OUTPUT     8

/* setMixer flags (cmd.flags). */
#define SF_MIXER_FLAG_MUTE 0x1
#define SF_MIXER_FLAG_SOLO 0x2

/* Fixed-size command image — never grows (fits the ring slot; 176 B incl.
   8 B alignment padding — content is 168 B). Slot size is a compile-time
   constant (D4: stays 176 B with the G3 command set — STOP(0) and
   EVALUATE_MIXER(7) carry no payload); the 10k-message threaded test asserts
   no gap and no overflow at capacity 256 (≈ 39 drains). */
typedef struct sf_cmd {
  int32_t  type;        /* 0 stop, 1 addNode, 2 removeNode, 3 addEdge,
                           4 removeEdge, 5 setMixer, 6 setPreset,
                           7 evaluateMixer, 8 setOutput */
  uint64_t seq;         /* monotonic, assigned by enqueue */
  char     id1[64];     /* node/edge uuid / new-node name (addNode); new output
                           node id (setOutput); NUL-terminated */
  char     id2[64];     /* second uuid (edge target / preset id); NUL-terminated */
  double   value;       /* gain_db (setMixer) */
  double   value2;      /* pan (setMixer) */
  int32_t  flags;       /* bit0 mute, bit1 solo; kind carries in port_a for addNode */
  int32_t  port_a;      /* from_port (addEdge) / node kind (addNode) */
  int32_t  port_b;      /* to_port (addEdge) */
} sf_cmd_t;

typedef struct sf_cmd_queue_s sf_cmd_queue_t; /* opaque */

/* Allocates the ring (NOT on the audio path — C2). */
sf_result_t sf_cmd_queue_create(sf_cmd_queue_t** out);
/* Frees the ring; NULL is a safe no-op. Never called by an audio callback. */
void sf_cmd_queue_destroy(sf_cmd_queue_t* q);
/* Producer side. Lock-free & malloc-free: single acquire load of head,
   release store of tail, fixed-slot copy. Overwrites cmd->seq with the next
   monotonic watermark. Full ring: rejects with SF_E_IO + WARN log
   "cmdqueue: overflow dropped seq=<n>"; the message is not enqueued and does
   NOT consume a seq number (a later retry of the same message reuses <n>), so
   accepted seqs stay gapless. No blocking, no drop-oldest. */
sf_result_t sf_cmd_queue_enqueue(sf_cmd_queue_t* q, const sf_cmd_t* cmd);
/* Consumer side. Lock-free & malloc-free: acquire load of tail, release
   store of head. Empty ring: SF_E_IO ("queue empty"), safe to retry. */
sf_result_t sf_cmd_queue_dequeue(sf_cmd_queue_t* q, sf_cmd_t* out);
/* Diagnostics/tests only (never on the audio path): in-flight command count. */
int32_t sf_cmd_queue_depth(const sf_cmd_queue_t* q);

/* Drain + apply: applies cmds[0..n) to p via the SAME code path as the
   synchronous mutators (impl helpers + ring of audit entries + modifiedAt
   bumps per cmd). Applies sequentially; on the first failing cmd it stops,
   copies the error message into err_buf (if err_cap > 0) and returns that
   cmd's code, with *applied = number applied so far. SF_E_INVALID_ARG for
   unsupported command types (including SF_CMD_STOP, SF_CMD_EVALUATE_MIXER and
   SF_CMD_SET_OUTPUT — all runner-only). Busy-rejects with
   SF_E_IO "project.busy: queue runner active" while a queue runner is active
   (C7) — the runner thread consumes the queue and owns the handle's mutation
   thread; use sf_queue_runner_stop+join (C8) before sync use. */
sf_result_t sf_graph_apply_batch(sf_project_t* p, const sf_cmd_t* cmds, size_t n,
                                 size_t* applied, char* err_buf, size_t err_cap);

/* ---------------------------------------------------------------------------
 * Threading / concurrency contract (§4.5 + G3 P4a D3-amd)
 *
 *  C1  SPSC: one producer, one consumer. In G3 the runner is the sole
 *      consumer while started; two consumers are undefined behavior.
 *  C2  Lock-free & malloc-free on push AND pop. create/destroy allocate and
 *      are never called by an audio callback.
 *  C3  A project handle keeps exactly one mutating thread. The queue only
 *      transports intent; the runner applies it on its own thread. The queue
 *      itself shares no state with the handle.
 *  C4  seq is a monotonically increasing watermark assigned on ACCEPTANCE;
 *      accepted command seqs are always dense (no gaps by construction). An
 *      overflowed command is rejected (SF_E_IO + WARN) WITHOUT consuming a
 *      seq number, so a producer that retries the same message after a
 *      transient full still delivers a gapless stream. depth() is for
 *      diagnostics.
 *  C5  [G3 P4a rewrite] The queue's live consumer is sf_queue_runner (the
 *      G2-promised G3 lane): the runner drains bounded batches and applies
 *      them on its own thread via the internal apply_batch_impl. The queue is
 *      no longer a dormant primitive — enqueued intent is applied
 *      asynchronously whenever a runner is started.
 *  C6  [G3 P4a rewrite] Sync sf_graph_apply_batch remains for non-RT callers
 *      but busy-rejects (SF_E_IO "project.busy: queue runner active") while a
 *      runner is active; it never runs concurrently with the runner. STOP(0)
 *      and EVALUATE_MIXER(7) are runner-only: the sync entry rejects both.
 *  C7  While a runner is started, the runner owns the handle's mutation
 *      thread (single-owner invariant enforced at the ABI boundary — D3-amd
 *      SEC-G3-1/-G3-3). The runner intercepts STOP (stops the drain — ORC-2),
 *      EVALUATE_MIXER (stores a bounded ≤8 KiB report — ORC-3) and
 *      SET_OUTPUT (forwards the target to the engine observer — G5 P1); all
 *      other commands go through apply_batch_impl. The runner NEVER writes
 *      proj->lastError (SEC-G3-4).
 *  C8  Lifecycle ordering: stop() then join() BEFORE any external access or
 *      destroy (runner destroy or sf_project_destroy) — the runner's epilogue
 *      publishes STOPPED only after its last batch. After stop+join the
 *      handle is owned by the caller again (sync use allowed; the lifecycle
 *      itself is one-shot — start accepts IDLE only). The forward race
 *      documented in D3-amd: the sync guard is a single atomic check, so a
 *      runner starting CONCURRENTLY with an external call can still race into
 *      it — callers must not start/stop a runner while other ABI calls on the
 *      same handle are in flight.
 * ------------------------------------------------------------------------- */
#ifdef __cplusplus
}
#endif