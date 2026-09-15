// SoundForge G4 P4 — sf_audio_engine: host-side audio engine (PLAN_G4 §4.2 D2,
// §4.5 D5, §6 P4). A virtual device callback driven by a synthetic clock
// (sf_audio_engine_tick) or an optional pacer thread (SF_AUDIO_ENGINE_PACE).
//
// Lifecycle (one-shot, mirroring sf_queue_runner):
//   create → configure → set_output → start → tick*/stop → join → destroy
//   (* or use PACE to drive ticks automatically)
//
// State machine: CREATED → RUNNING → STOPPING → STOPPED. Only CREATED accepts
// configure/set_output. Only RUNNING accepts tick. stop() transitions to
// STOPPING (an async-stop request). join() reaps the pacer & runner theads and
// moves to STOPPED. The engine cannot be restarted.
//
// Caller-lifetime hazard (R-D, sec-g4-10): the engine borrows q and p (like the
// runner does); both must outlive the engine. sf_audio_engine_destroy must be
// called before sf_project_destroy. An engine that outlives its project is
// caller UB (same posture as sf_queue_runner).
//
// double-destroy of a live handle is caller UB (mirrors sf_queue_runner).
// destroy(NULL) → SF_OK safe no-op.
//
// THREAD SAFETY:
//   - create, configure, set_output, start, stop, join, destroy:
//     single-threaded by caller contract (same as sf_queue_runner).
//   - tick and the pacer must NOT be used concurrently (the engine returns
//     SF_E_IO on tick while SF_AUDIO_ENGINE_PACE is active — best-effort).
//   - meter_json / reset_meters are safe in ANY state and may be called
//     concurrently with a ticking pacer (SEC-G4-02 choice (a): a single engine
//     std::mutex guards process/latch/reset AND meter_json/reset_meters).
//
// The float* const* in sf_audio_engine_io_t is a host-only device protocol
// that never crosses JNI (no JNI symbol exposes it). JNI grep stays 26.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "soundforge/sf_types.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_command_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque engine handle. */
typedef struct sf_audio_engine_s sf_audio_engine_t;

/* Host device callbacks: read input and write rendered output. Both run on the
   engine's render lane (tick caller or pacer thread). They must be
   lock-free/alloc-free (the render path is allocation-free). */
typedef struct sf_audio_engine_io {
  void* user;
  /* Fill the next input block. `in[c]` has `frames` floats (deinterleaved).
     Return SF_OK to use the filled input, or SF_E_IO to render the block as
     silence (zeros). May be NULL (→ silence). */
  sf_result_t (*read)(void* user, float* const* in, int32_t channels,
                      int32_t frames);
  /* Consume the rendered output block. `out[c]` has `frames` floats.
     May be NULL (output is discarded). The engine does NOT propagate the
     return value (block already rendered). */
  sf_result_t (*write)(void* user, const float* const* out, int32_t channels,
                       int32_t frames);
} sf_audio_engine_io_t;

/* Engine configuration; set via sf_audio_engine_configure (CREATED only).
   Default (before configure): sample_rate=48000, channels=2,
   max_block_frames=512, io={nullptr,nullptr,nullptr}. */
typedef struct sf_audio_engine_config {
  int32_t sample_rate;        /* > 0; e.g. 48000 */
  int32_t channels;           /* must be 2 (G4); other → SF_E_INVALID_ARG */
  int32_t max_block_frames;   /* 1..512 (kBlockMaxSamples) */
  sf_audio_engine_io_t io;    /* may be zero-initialized (NULL callbacks) */
} sf_audio_engine_config_t;

/* Flags for sf_audio_engine_start. */
#define SF_AUDIO_ENGINE_PACE 0x1u

/* ─────────────────────────────────────────────────────────────────────────────
 * 11 C exports
 * ─────────────────────────────────────────────────────────────────────────── */

/* 1 — Bind queue q to project p and allocate the engine. Atomically claims
   SfProject::audioEngine (CAS nullptr → e) — the real single-engine guard
   (amendment R-D). Requires p.runnerState == IDLE. Fails with
   SF_E_INVALID_ARG ("audio engine: create: null argument") on NULL arguments,
   SF_E_NOMEM on allocation failure, and SF_E_IO if an engine is already
   attached ("audio engine: create: engine already attached") or if the
   project is not idle. On any failure the claim is rolled back. */
sf_result_t sf_audio_engine_create(sf_audio_engine_t** out, sf_cmd_queue_t* q,
                                   sf_project_t* p);

/* 2 — Frees the engine handle; NULL → SF_OK safe no-op. Requires engine state
   not-RUNNING and not-STOPPING (destroy while those states are active returns
   SF_E_IO and does NOT free the handle). Exact ordering (R-D):
   join pacer → runner.stop → runner.join → detach observer →
   sf_queue_runner_destroy → free snapshot store → free engine →
   release the audioEngine claim. */
sf_result_t sf_audio_engine_destroy(sf_audio_engine_t* e);

/* 3 — Configure sample_rate, channels, max_block_frames and io callbacks.
   Validates: sample_rate > 0, channels == 2, max_block_frames ∈ [1, 512].
   Returns SF_E_IO when not in CREATED state, SF_E_INVALID_ARG on null
   arguments or invalid values. */
sf_result_t sf_audio_engine_configure(sf_audio_engine_t* e,
                                      const sf_audio_engine_config_t* cfg);

/* 4 — Set the output node id for the render plan. NULL/"" selects "no output"
   (renders silence) as a VALID configuration (SF_OK, ORC-G4-06). Unknown
   target ids compile a valid plan with out_index==-1 (renders silence).
   Returns SF_E_IO when not in CREATED state. */
sf_result_t sf_audio_engine_set_output(sf_audio_engine_t* e,
                                       const char* out_node_id);

/* 5 — Ordered gate + snapshot #0 + rollback (SEC-G4-05):
   (1) acquire-load p.runnerState — reject SF_E_IO if runner is alive or
       runnerState != IDLE (a standalone runner could have been started after
       create);
   (2) compile + publish snapshot #0 from the IDLE document (non-fatal plan
       compile failure sets planValid:false and renders silence);
   (3) attach the publish observer to the internal runner BEFORE
       sf_queue_runner_start;
   (4) start the internal runner, then optionally the pacer thread (when
       flags & SF_AUDIO_ENGINE_PACE).
   On runner-start or pacer-spawn failure: stop+join what was started, detach
   the observer, restore the snapshot store, and return SF_E_IO (engine stays
   CREATED so destroy still works). One-shot: RUNNING state → SF_E_IO
   "audio engine: already started". Unknown flags are ignored. */
sf_result_t sf_audio_engine_start(sf_audio_engine_t* e, uint32_t flags);

/* 6 — Request a stop: CAS RUNNING → STOPPING, signals the pacer and calls
   sf_queue_runner_stop on the internal runner. Idempotent; non-RUNNING is a
   safe SF_OK no-op. */
sf_result_t sf_audio_engine_stop(sf_audio_engine_t* e);

/* 7 — Reap the pacer and runner threads. Joins the pacer thread (if any) and
   sf_queue_runner_join, then transitions engine state to STOPPED. Blocking
   but bounded if stop() was called first (mirroring the runner contract).
   Idempotent. */
sf_result_t sf_audio_engine_join(sf_audio_engine_t* e);

/* 8 — Deterministic synthetic clock: render `frames` through the current plan
   (no wall clock). Requires engine state RUNNING. Returns SF_E_IO when
   not RUNNING or when the pacer is active (pace_vs_tick concurrency guard).
   Returns SF_E_INVALID_ARG when frames==0 or frames > max_block_frames.
   channels must be 2 (validated at configure). The io.read callback fills
   the input block (NULL→zeros, SF_E_IO→silence); io.write delivers the
   output block (NULL→skip, error→skip). True-peak meters are updated after
   render. Always returns SF_OK when preconditions are met (silence is valid). */
sf_result_t sf_audio_engine_tick(sf_audio_engine_t* e, size_t frames);

/* 9 — Passthrough to the internal sf_queue_runner's last_report. Same contract
   as sf_queue_runner_last_report: SF_E_IO "runner.lastReport: no report yet"
   before any sf_cmd_evaluate_mixer completes; SF_E_INVALID_ARG on NULL
   e/buf or cap==0; SF_E_NOMEM when cap too small. The returned content is
   byte-identical to what sf_queue_runner_last_report would return for the
   same runner state (proved by P4 test). meter_json is NEVER embedded in
   this report. */
sf_result_t sf_audio_engine_last_report(const sf_audio_engine_t* e, char* buf,
                                        size_t cap);

/* 10 — Per-channel true-peak meter JSON (PLAN_G4 §4.3 D3, §4.6 D6). Safe in
   any state. Keys: channels, oversample:4, blocksRendered, truePeakLinear[2],
   truePeakDb[2] (JSON null when the channel is silent), clipped[2], planValid.
   Buffer contract (SEC-G4-01): NULL e/buf or cap==0 → SF_E_INVALID_ARG;
   cap too small → SF_E_NOMEM AND buf[0]='\0' (never a partial payload);
   serialization is bounded, locale-independent (std::to_chars), and performs
   exactly ONE sized copy. */
sf_result_t sf_audio_engine_meter_json(const sf_audio_engine_t* e, char* buf,
                                       size_t cap);

/* 11 — Zero the per-channel true-peak latches only (keeps the FIR tails so
   the next block is in steady state). blocksRendered is NOT reset. Safe in
   any state. */
sf_result_t sf_audio_engine_reset_meters(sf_audio_engine_t* e);

#ifdef __cplusplus
}
#endif