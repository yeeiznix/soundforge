// SoundForge G2 P5 — lock-free SPSC command queue (§4.4, §4.5).
// Transports *intent*: commands are applied on the project's owner thread via
// sf_graph_apply_batch (C3). G2 ships this as a tested primitive — no live
// audio lane consumes it until G3 (C5/C6).
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "soundforge/sf_types.h"
#include "soundforge/sf_project.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Command types carried by the queue. 7 (evaluateMixer) is reserved for the
   G3 audio lane; sf_graph_apply_batch rejects it (query, not a mutation). */
#define SF_CMD_ADD_NODE       1
#define SF_CMD_REMOVE_NODE    2
#define SF_CMD_ADD_EDGE       3
#define SF_CMD_REMOVE_EDGE    4
#define SF_CMD_SET_MIXER      5
#define SF_CMD_SET_PRESET     6
#define SF_CMD_EVALUATE_MIXER 7

/* setMixer flags (cmd.flags). */
#define SF_MIXER_FLAG_MUTE 0x1
#define SF_MIXER_FLAG_SOLO 0x2

/* Fixed-size command image — never grows (fits the ring slot; 176 B incl.
   8 B alignment padding — content is 168 B). Slot size is a compile-time
   constant; the 10k-message threaded test asserts no gap and no overflow
   at capacity 256 (≈ 39 drains). Revisit size with the G3 command set. */
typedef struct sf_cmd {
  int32_t  type;        /* 1 addNode, 2 removeNode, 3 addEdge, 4 removeEdge,
                           5 setMixer, 6 setPreset, 7 evaluateMixer */
  uint64_t seq;         /* monotonic, assigned by enqueue */
  char     id1[64];     /* node/edge uuid / new-node name (addNode); NUL-terminated */
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
   unsupported command types (including SF_CMD_EVALUATE_MIXER). */
sf_result_t sf_graph_apply_batch(sf_project_t* p, const sf_cmd_t* cmds, size_t n,
                                 size_t* applied, char* err_buf, size_t err_cap);

/* ---------------------------------------------------------------------------
 * Threading / concurrency contract (§4.5)
 *
 *  C1  SPSC: one producer, one consumer. Two producers are undefined behavior.
 *  C2  Lock-free & malloc-free on push AND pop. create/destroy allocate and
 *      are never called by an audio callback.
 *  C3  A project handle keeps exactly one mutating thread. The queue only
 *      transports intent; sf_graph_apply_batch applies it on the owner
 *      thread. The queue itself shares no state with the handle.
 *  C4  seq is a monotonically increasing watermark assigned on ACCEPTANCE;
 *      accepted command seqs are always dense (no gaps by construction). An
 *      overflowed command is rejected (SF_E_IO + WARN) WITHOUT consuming a
 *      seq number, so a producer that retries the same message after a
 *      transient full still delivers a gapless stream. depth() is for
 *      diagnostics.
 *  C5  G2 ships the queue as a tested primitive, not yet on a live audio
 *      lane (no RT callback exists until G3).
 *  C6  In G3 the audio callback drains the queue and applies via
 *      sf_graph_apply_batch. The G2 screens bypass the queue and call the
 *      synchronous mutators directly.
 * ------------------------------------------------------------------------- */
#ifdef __cplusplus
}
#endif