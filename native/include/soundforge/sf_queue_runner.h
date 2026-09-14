// SoundForge G3 P4a — sf_queue_runner: the live RT queue-drain lane (PLAN_G3
// §4.3 D3-amd). A runner BINDS a queue (producer side, e.g. the audio lane)
// to a project handle and, once started, becomes the handle's sole mutating
// thread: it drains bounded batches off the queue and applies them via the
// internal apply_batch_impl on its own thread.
//
// Contract summary:
//   - create: binds q + p; requires the project's runner state IDLE.
//   - start:  one-shot. CAS IDLE->RUNNING and spawns the thread; any other
//             state -> SF_E_IO "project.runner: already active".
//   - stop:   async flag (CAS RUNNING->STOPPING); the runner drains its current
//             batch, then exits. Idempotent / safe no-op when not RUNNING.
//   - join:   reaps the thread; idempotent and a safe no-op if never started.
//   - last_report: bounded single-slot copy of the latest EVALUATE_MIXER(7)
//             result. Overwrite semantics — only the most recent report is
//             kept; no report yet -> SF_E_IO.
//   - destroy: frees the runner handle. Requires IDLE/STOPPED (i.e. not
//             RUNNING/STOPPING); a started-but-unjoined runner is auto-joined
//             first so std::thread's destructor never aborts.
//
// The runner NEVER writes proj->lastError (SEC-G3-4); after stop+join the
// handle is caller-owned again (C8) and sync use is allowed. The runner handle
// must not outlive its queue OR its project handle, and vice versa: the project
// handle must not be destroyed while a runner is attached (SEC-G3-2). The
// lifecycle is one-shot: START accepts IDLE only, so nothing resets STOPPED
// back to IDLE and a runner cannot be restarted (ORC-3).
#pragma once

#include <stddef.h>

#include "soundforge/sf_types.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_command_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sf_queue_runner_s sf_queue_runner_t; /* opaque */

/* Binds queue q to project p and allocates the runner. Host-only: the caller
   owns both q and p for the runner's whole lifetime. Fails with
   SF_E_INVALID_ARG ("runner.create: null argument") on NULL arguments and
   SF_E_IO ("runner.create: project not idle") if p already has a runner
   attached (RUNNING/STOPPING/STOPPED — a project handle is single-runner for
   its lifetime). */
sf_result_t sf_queue_runner_create(sf_queue_runner_t** out, sf_cmd_queue_t* q,
                                   sf_project_t* p);

/* Spawns the drain thread (one-shot). CAS IDLE->RUNNING; any other state
   returns SF_E_IO "project.runner: already active". A failed thread spawn
   restores IDLE and returns SF_E_IO. */
sf_result_t sf_queue_runner_start(sf_queue_runner_t* r);

/* Requests a drain stop: CAS RUNNING->STOPPING. The runner finishes the batch
   it is applying, honors any in-buffer SF_CMD_STOP barrier, then exits without
   draining further. Non-RUNNING states are an idempotent SF_OK no-op. */
sf_result_t sf_queue_runner_stop(sf_queue_runner_t* r);

/* Reaps the runner thread (blocking, bounded by the current batch). Idempotent
   and a safe no-op if the runner was never started. */
sf_result_t sf_queue_runner_join(sf_queue_runner_t* r);

/* Copies the latest EVALUATE_MIXER report into buf (NUL-terminated) when
   cap > 0. Reports are capped at 8 KiB at STORE time; an over-cap dump is
   replaced by the longest valid JSON prefix plus a `"_truncated": true`
   sentinel member (ORC-3). Returns SF_E_IO ("runner.lastReport: no report
   yet") when no EVALUATE has completed, SF_E_INVALID_ARG on NULL r/buf or
   cap == 0, and SF_E_NOMEM ("runner.lastReport: buffer too small") if the
   stored report does not fit cap. */
sf_result_t sf_queue_runner_last_report(const sf_queue_runner_t* r, char* buf,
                                        size_t cap);

/* Frees the runner handle; NULL is a safe no-op. Requires the runner to be
   IDLE or STOPPED (a started but unjoined runner is auto-joined first). While
   RUNNING/STOPPING it sets the project's last_error to
   "project.destroy: queue runner active", logs ERROR and returns SF_E_IO
   (the handle is NOT freed) — stop+join then destroy. */
sf_result_t sf_queue_runner_destroy(sf_queue_runner_t* r);

#ifdef __cplusplus
}
#endif
