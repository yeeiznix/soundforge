# SoundForge — Gate G5 Plan: Live Engine Control + G4 Hardening —
# `SF_CMD_SET_OUTPUT` Flow + Plan Recompile/Publish + Residual Hardening

> **Status: PLANNING — draft scratch document for orchestrator review.**
> This document is the phase-0 plan contract for Gate G5. Reviewed and amendments
> applied per gate process before P1 may begin (orchestrator tags `plan(g5)` after
> approval). The plan specifies the headline live-control command, the query/publish
> flow, the hardening lanes for G4-4/5/6/8/9 residuals, and the phase structure
> (P0…Pn, one commit per phase, each leaving ctest + pytest green).
> **Supersedes:** `docs/PLAN_G0.md`…`PLAN_G4.md` for G5 scope only. G0–G4 remain
> the contract for everything not changed here. G4 tag: `g4-complete`
> (`c0c6040`).
> **Deliverable:** a **live engine control command** (`SF_CMD_SET_OUTPUT`) flowing
> through the queue into the runner, triggering a **plan recompile and publish**
> without the runner touching the engine instance, so the next rendered block
> reads the new target. Plus **hardening lanes** for the G4 residuals: memory
> bounds (G4-5), atomic<double> documentation (G4-6), project-destroy posture
> (G4-8), and allocator alignment (G4-9).

---

## 1. G5 Scope & Non-Goals

### 1.1 Goal

G4 shipped a host-testable audio engine with a live queue runner and a 4-slot
lock-free snapshot store (the "plan snapshot publish" observer). However, the
**output target is fixed at engine start** (via `set_output`, CREATED-only) —
G4-4 residual says "revisit with a live-control command". G5 closes that gap
with a **single-writer command** (`SF_CMD_SET_OUTPUT`, cmd type **8**) that the
runner applies on its own thread, triggering a fresh plan compile and publish
without touching the engine's live state. The next rendered tick reads the new
target.

G5 also hardens four remaining G4 residuals (documented, non-blocking):
- **G4-5:** Plan memory scales with node count (~4 KB/node, 4 slots → ~16 KB/node
  live); a **node cap or pre-size check** before pool build reduces fragmentation.
  **Decision at P2:** justify choice (defensive only; no functional change).
- **G4-6:** `std::atomic<double>` used for meter fields; document the assumption
  (aarch64/x86 lock-free; not guaranteed elsewhere). **Decision at P3 (docs):**
  verify generation guard, state the constraint.
- **G4-8:** `sf_project_destroy` (void, SEC-G3-2 pattern) cannot see an attached
  engine in CREATED state. **Decision at P4:** refuse-free hardening (optional);
  if chosen, test cleanup ordering.
- **G4-9:** `alloc_counter` replaces only default-aligned `operator new` (no
  over-aligned overloads). **Decision at P1 (comment):** add aligned overloads
  XOR explicitly comment the precondition.

### 1.2 G5 MUST deliver

| Pillar | Scope | Deliverable |
|---|---|---|
| **Live output command** | `native/src/core/command_queue_thread.cpp` | New command type `SF_CMD_SET_OUTPUT` (cmd 8); runner intercepts it, extracts the target id, and **forwards it to the engine through the internal observer callback** (never touching engine internals) WITHOUT triggering `apply_batch_impl`; the engine's callback recompiles the plan with the new target and feeds the next tick |
| **Command constant** | `native/include/soundforge/sf_command_queue.h` | `#define SF_CMD_SET_OUTPUT 8` (new, public constant; no wire/schema change; command plane not `SF_E_*`) |
| **Publish without apply** | `native/src/core/command_queue_thread.cpp` | Runner intercepts `SF_CMD_SET_OUTPUT` (like `SF_CMD_STOP` and `SF_CMD_EVALUATE_MIXER`) before `apply_batch_impl` and fires the observer's **new `set_output` callback** (`user`, `cmd.id1`). The runner never dereferences the engine and never reads `sf_audio_engine_s`. |
| **Engine live-retarget** | `native/src/audio/audio_engine.cpp` (EDIT — minor) | The engine installs a second observer callback (`engine_set_output`) that writes `e->out_node_id` and re-publishes from `e->proj->doc.signalGraph` — both on the runner thread, where the doc is owned. No new export, no new field, no ABI change; `sf_audio_engine_s` stays private to its TU. |
| **Observer extension (internal)** | `native/src/core/sf_internal.hpp` (EDIT — internal C++ only) | `RunnerObserver` gains `void (*set_output)(void* user, const char* target) = nullptr;`, mirroring the existing `publish` pointer. No public header, no export, no JNI, no wire/schema change (G4 added the struct the same way). |
| **Memory hardening (optional)** | `native/src/graph/render_plan.cpp` (optional P2) | Node cap constant or pre-size check before pool build; if implemented, test for OOM boundary behavior and fragment reduction. If declined, document G4-5 stands (no functional change). |
| **Atomic<double> documentation** | `native/include/soundforge/sf_audio_engine.h` (P3, header comment) | State the assumption: `std::atomic<double>` is lock-free on aarch64/x86 (not portable). Generation guard confirmed (meter value + version counter to detect torn reads). |
| **Project-destroy hardening (optional)** | `native/src/core/project.cpp` (optional P4) | If chosen: `sf_project_destroy` checks `audioEngine != nullptr` and refuses with `SF_E_IO` + error log (G4-8 posture match). If declined, document the borrowing hazard in the engine header + test cleanup ordering. |
| **Allocator alignment** | `tests/unit/alloc_counter.cpp` (P1 amendment) | Add `operator new(size, align_val_t)` overloads OR add a comment explaining the default-alignment-only precondition (G4-9 assertion). Chose one; justify in P1 acceptance. |
| **Integration test** | `tests/unit/test_audio_engine.cpp` (EDIT — add live-control case) | Live output change while RUNNING: enqueue `SF_CMD_SET_OUTPUT("new_out")`, drain via STOP barrier, next tick renders the new target; meter and audit trail confirm the change; no `project.migrate` entry; schemaVersion 2. |

### 1.3 G5 MUST NOT deliver (defer, leave stubs/unchanged)

- **New `SF_E_*` codes** — reuse existing codes (`SF_E_INVALID_ARG`, `SF_E_IO`, `SF_E_NOMEM`). Command constants (cmd type 8) are NOT error codes.
- **Schema / wire change** — **schemaVersion stays 2**; `project_schema.json`,
  golden files, and both SHA-256 pins are **untouched**; no new document key; no
  migration. G5 adds *zero* wire surface.
- **New JNI** — `app/**` untouched; export grep stays **26**. The
  `SF_CMD_SET_OUTPUT` command is host queue only; no Android `NativeBridge`
  mirror, no new Kotlin.
- **`float[]` across JNI** — no new IO surface; engine host-only (G4 posture
  preserved).
- **Runtime reconfigure while paused/stopped** — `set_output` remains CREATED-only
  for the static path; live-control is RUNNING-only via the queue. No hybrid
  mutation.
- **Per-command retargeting latency SLA** — G5 adds no determinism guarantee; the
  next rendered tick after the command drains is when the new target takes
  effect (same delay model as other queue commands).
- **Backward compatibility exception** — `SF_CMD_SET_OUTPUT` is a new command
  type; old clients are unaware of it. The queue rejects it with the default
  switch case (unsupported type → `SF_E_INVALID_ARG`), no silent drop, so
  forward compatibility is safe (old clients simply don't use the command).

### 1.4 Gating rule

Same as G0–G4: *new* files implement their bounded contract only. New G5 source
files target ≤100 LOC *each* (command handler + optional hardening patch).
Existing files (engine, runner, allocator) receive small, focused amendments.

---

## 2. File / Module Breakdown

Canonical root: `/root/project/soundforge/`. Tags: **NEW** / **EDIT** / **KEEP**.

```
soundforge/
├── native/
│   ├── CMakeLists.txt                      # EDIT (P5) — SF_BUILD_VERSION "0.1.0-g5"
│   ├── include/soundforge/
│   │   ├── sf_command_queue.h              # EDIT — + #define SF_CMD_SET_OUTPUT 8
│   │   ├── sf_audio_engine.h               # EDIT — + atomic<double> assumption comment (P3)
│   │   └── sf_version.h                    # EDIT (P5) — SF_ENGINE_VERSION_SUFFIX "-g5"
│   ├── src/dsp/
│   │   └── dsp_internal.hpp                # KEEP — unchanged (leaf invariant)
│   ├── src/graph/
│   │   ├── render_plan.cpp                 # EDIT (optional) — node cap or pre-size check (P2)
│   │   └── CMakeLists.txt                  # KEEP unless P2 adds a new constant
│   ├── src/audio/
│   │   └── audio_engine.cpp                # EDIT — + engine_set_output callback (runner-thread publish)
│   ├── src/core/
│   │   ├── command_queue_thread.cpp        # EDIT — intercept SF_CMD_SET_OUTPUT, fire set_output callback
│   │   ├── project.cpp                     # EDIT (optional) — G4-8 refuse-free hardening (P4)
│   │   └── sf_internal.hpp                 # EDIT — + RunnerObserver::set_output callback ptr (internal only)
│   └── src/measurement/
│       └── true_peak.hpp                   # EDIT (optional) — atomic<double> comment (P3)
├── tests/
│   └── unit/
│       ├── alloc_counter.cpp               # EDIT — aligned overloads XOR comment (P1)
│       ├── test_audio_engine.cpp           # EDIT — + live-control case
│       └── CMakeLists.txt                  # KEEP (link list unchanged)
└── docs/
    ├── PLAN_G5.md                          # this document (P0)
    └── RELEASE_NOTES_G5.md                 # NEW — gate artifact (P5)
```

**KEEP (explicitly untouched):** `sf_types.h` (no new error codes), `sf_project.h`
signatures, `sf_queue_runner.h` signatures, `sf_graph.h` signatures, `project_schema.json`,
all golden files (both SHA-256 pins), `app/**` (no Kotlin, no JNI), all DSP
kernels, `dsp_internal.hpp` (leaf invariant).

> **Bound check:** if a path is not listed above, do not create or edit it in G5.
> Command constants are public and go in `sf_command_queue.h`; hardening patches
> are internal amendments (no new exports).

---

## 3. Architecture Decisions

Each decision: **Decision** / **Rationale** / **Acceptance**.

### 3.1 D1 — Live output command: `SF_CMD_SET_OUTPUT` (cmd type 8)

**Problem.** G4-4 residual: "Output target is fixed pre-start; no runtime
retargeting." The engine's `out_node_id` is set via `set_output()` (CREATED-only),
then compiled into snapshot #0 at `start()`. To change the target while RUNNING,
we need a queue command that the runner applies on its own thread (it owns the
doc and can read `signalGraph`), compiles a fresh plan with the new target, and
publishes it to the snapshot store.

**Decision.** Add **`SF_CMD_SET_OUTPUT` (command type 8)** to the queue. The
runner **intercepts** it (like `SF_CMD_STOP` and `SF_CMD_EVALUATE_MIXER`), before
`apply_batch_impl`:

```c
typedef struct sf_cmd {
  int32_t  type;        /* ..., 8 = setOutput */
  uint64_t seq;
  char     id1[64];     /* new output node id (setOutput) */
  char     id2[64];     /* unused in setOutput */
  double   value;       /* unused */
  double   value2;      /* unused */
  int32_t  flags;       /* unused */
  int32_t  port_a;      /* unused */
  int32_t  port_b;      /* unused */
} sf_cmd_t;             /* 176 B, unchanged (command plane not SF_E_*) */
```

**Interception flow (runner thread, command_queue_thread.cpp:run_loop):**

1. Dequeue command.
2. Check type == `SF_CMD_SET_OUTPUT` (cmd 8, new case in the type switch).
3. **Runner-only, opaque:** extract `cmd.id1` (new target node id) and fire the
   observer's `set_output` callback with it:
   `self->observer.set_output(self->observer.user, cmd.id1)`. The runner **never
   dereferences `user`** and **cannot name `sf_audio_engine_s`** — that struct is
   private to `audio_engine.cpp` (see the constraint note below).
4. The **engine-side callback** (`engine_set_output`, defined in
   `audio_engine.cpp` next to `snapshot_publish`) does the mutation: it stores the
   new target in `e->out_node_id` and calls `e->store->publish(e->proj->doc.signalGraph,
   e->out_node_id, err)` directly (the same publish logic used by
   `snapshot_publish`). If publish succeeds, DEBUG "live output changed"; if
   compile fails, store `plan_valid=false` and log ERROR (last valid plan
   retained).
5. Do NOT call `apply_batch_impl` (this is a control command, not a graph
   mutation).
6. Do NOT increment `applied` counter (no batch mutation count).
7. Continue the drain loop.

**Hard constraint the design must respect (found during P0 grounding).**
`sf_audio_engine_s` is defined **only** in `native/src/audio/audio_engine.cpp`
(line 73) and is *not* visible to `native/src/core/command_queue_thread.cpp`.
The G4 `RunnerObserver` contract is explicit that `user` is *opaque — never
dereferenced here* (`sf_internal.hpp:539`). Therefore the runner **must not**
`static_cast<sf_audio_engine_s*>(observer.user)`; doing so does not compile.
G5 routes the retarget through the observer callback pointer instead, exactly as
the existing `publish` pointer already routes the graph. This keeps the engine
struct private and the runner free of engine internals.

**Synchronization.** The runner thread holds `proj->doc` ownership, so reading
`signalGraph` and writing `engine->out_node_id` are safe (no concurrent mutation).
The engine's `out_node_id` is otherwise **read-only from the observer context**
(the snapshot store's publish callback, which runs on the runner thread). The
only other access is inside `start()` when the static path (caller owns the doc)
compiles snapshot #0 and the engine reads its own `out_node_id` — this is safe
because the runner has not started yet (sequential). All `out_node_id` reads and
writes therefore occur either in the CREATED/`start()` phase or on the runner
thread; **no `std::atomic` is needed** because the runner's publish callback and
the retarget callback are the same thread.

**Ownership note.** The runner never dereferences `observer.user` in either the
`publish` or the new `set_output` path; `user` is handed straight back to the
engine callback. This preserves the G4 opaque-handle discipline and avoids
exposing `sf_audio_engine_s` to the core TU.

**Rationale.** This design:
- Preserves the G3 single-owner rule: the runner applies all control + mutation
  intent.
- Keeps the engine's `out_node_id` mutable only on the runner thread (no new
  synchronization primitive).
- Reuses the snapshot store's publish path (no new observation/handoff).
- Produces a fresh plan on the next publish cycle (deterministic, after the
  drain).
- The next tick reads the new plan from the snapshot store (same as any G4
  mutation).

**Acceptance.**
- `SF_CMD_SET_OUTPUT` (cmd 8) is defined as a public constant in
  `sf_command_queue.h`.
- The runner intercepts it before `apply_batch_impl`, extracts the target id
  from `cmd.id1`, fires the internal `observer.set_output` callback, and the
  engine-side callback updates `out_node_id` and re-publishes.
- The runner does **not** name `sf_audio_engine_s`; `RunnerObserver::user` stays
  opaque (G4 discipline preserved). Verified by grep: no `sf_audio_engine_s`
  reference in `native/src/core/`.
- No new export, no new error code, no schema change.
- The engine's `out_node_id` member remains CREATED-only writable via `set_output`
  (static path) and runner-thread-writable via the interception (dynamic path).
- Live-control test: enqueue `SF_CMD_SET_OUTPUT`, drain via STOP barrier, next
  tick renders the new target; meter + audit confirm.

### 3.2 D2 — Hardening lane decision matrix (G4-4/5/6/8/9)

Four residuals are eligible for hardening (optional; G5 is a gate to **decide**
and implement chosen lanes; none are blocking). Each hardening lane is
**one commit per phase**, isolated and non-blocking.

| Residual | Issue | G5 Scope | Decision Rule | Phase |
|---|---|---|---|---|
| **G4-4** | Output fixed pre-start | DONE (D1: live-control command) | Live `SF_CMD_SET_OUTPUT` completes the contract | — |
| **G4-5** | Plan memory ~4 KB/node × 4 slots = ~16 KB/node; OOM contained via `planValid:false` + last-valid retention | OPTIONAL | Implement a **node cap constant** (e.g., `kMaxNodes = 10000` per graph) OR a **pre-size check before pool build** (e.g., `nodes.size() > kMaxNodes` → compile returns false + error). Justify choice: cap guards fragmentation without changing semantics (compile fails early, `planValid:false`, silence rendered). If declined: G4-5 stands as-is (memory bounded by doc size, worst case 8 MiB). | P2 (optional hardening lane) |
| **G4-6** | `std::atomic<double>` meter fields (lock-free on aarch64/x86; not portable) | OPTIONAL | Add a **comment in `sf_audio_engine.h`** stating the assumption: "Meter fields use `std::atomic<double>`, which is lock-free on aarch64/x86 (IA-64) but not guaranteed elsewhere. Platforms without lock-free 64-bit atomics may see contention." Confirm the generation guard (version counter + meter values) prevents torn reads. If declined: document the constraint in `RELEASE_NOTES_G5.md` as a platform limitation (no code change). | P3 (optional documentation) |
| **G4-8** | `sf_project_destroy` cannot see an attached engine; same posture as G3 IDLE-runner hole | OPTIONAL | Option A: **Refuse-free hardening** — `sf_project_destroy` checks `audioEngine != nullptr` and returns `SF_E_IO` with error log "audio engine attached; call sf_audio_engine_destroy first" (mirrors the runner guard). Requires test cleanup (destroy engine, *then* project). Option B: Document the borrowing hazard in `sf_audio_engine.h` (engine must be destroyed before project) and add a test asserting cleanup order. Justify choice: A hardens the ABI and catches misuse; B documents the contract. If declined: G4-8 stands (application responsibility). | P4 (optional hardening lane) |
| **G4-9** | `alloc_counter` replaces only default-aligned `operator new`; over-aligned allocations evade the zero-alloc assertion | OPTIONAL | Option A: Add **aligned overloads** (`operator new(size_t, std::align_val_t)` + `operator delete(void*, std::align_val_t)`) to the counter. Option B: Add a **comment** in `alloc_counter.cpp` explaining the default-alignment-only scope: "This counter is used by zero-alloc hot-path tests. It instruments default-aligned `operator new` only; over-aligned allocations evade this counter. The zero-alloc contracts assume no over-alignment on the hot path (verified by source scan)." Justify choice: A is more robust; B is simpler and documents the scope. | P1 (amendment; tiny patch) |

### 3.3 D3 — Command interception: where to add the `SF_CMD_SET_OUTPUT` case

**Decision.** In `command_queue_thread.cpp`, **at the top of the dequeue loop in
`run_loop()`** (same place as `SF_CMD_STOP` and `SF_CMD_EVALUATE_MIXER`),
**before** the `batch[n++] = cmd;` line:

```cpp
if (cmd.type == SF_CMD_SET_OUTPUT) {
  // Control: live output retarget. Hand the new target id to the engine's
  // observer callback (opaque `user`; runner never dereferences it) so the
  // engine can update out_node_id and re-publish from the doc it owns.
  if (self->observer.set_output != nullptr) {
    try {
      self->observer.set_output(self->observer.user, cmd.id1);
    } catch (const std::exception& e) {
      log_line(SF_LOG_ERROR, "runner",
               (std::string("observer set_output failed: ") + e.what()).c_str());
    } catch (...) {
      log_line(SF_LOG_ERROR, "runner", "observer set_output failed: unknown");
    }
  }
  log_line(SF_LOG_DEBUG, "runner", "live output retarget");
  continue;  // control command: do NOT apply as a mutation
}
```

Engine side (`audio_engine.cpp`, next to `snapshot_publish`):

```cpp
// Live-output observer (G5 P1): fired on the runner thread when a
// SF_CMD_SET_OUTPUT command drains. The runner owns the doc here, so reading
// proj->doc.signalGraph and rewriting out_node_id are single-threaded.
void engine_set_output(void* user, const char* target) {
  auto* e = static_cast<sf_audio_engine_s*>(user);
  e->out_node_id = target ? target : "";
  std::string err;
  const bool ok = e->store->publish(e->proj->doc.signalGraph, e->out_node_id, err);
  if (ok) {
    e->plan_valid.store(true, std::memory_order_release);
    log_line(SF_LOG_DEBUG, "engine", "live output changed");
  } else {
    e->plan_valid.store(false, std::memory_order_release);
    log_line(SF_LOG_ERROR, "engine", ("plan compile failed: " + err).c_str());
  }
}
```

…and the engine installs it in `start()` alongside the existing publish observer
(`obs.set_output = &engine_set_output;` before `sf_queue_runner_set_observer`).

**Rationale.** This follows the existing pattern for control commands (STOP,
EVALUATE) and keeps the interception logic isolated and localized. It also keeps
the opaque `RunnerObserver::user` contract intact: the runner forwards the target
string and never names or reaches into `sf_audio_engine_s`.

---

## 4. Schema, Wire & Version Impact

### 4.1 Wire delta — **none**

G5 adds **no** document key, **no** command type on the wire (command constants are
runtime only), **no** error code, and **no** migration. `schemaVersion` stays **2**;
`project_schema.json` byte-identical to `schema_golden_v2.json`; both SHA-256 pins
untouched; `schema_golden_v1.json` untouched. The drift `diff` in §7 must remain empty.
**This is a deliberate posture:** G5 is a pure live-control / hardening gate.

### 4.2 Version stamp

G5 lands **`0.1.0-g5`** (suffix convention, carried from G3/G4; no bumps to MAJOR/MINOR):
- `sf_version.h` suffix (`SF_ENGINE_VERSION_SUFFIX` macro: `-g5`).
- `native/CMakeLists.txt` default (`SF_BUILD_VERSION "0.1.0-g5"`).
- `native/src/core/version.cpp` fallback string (`"0.1.0-g5"`) + comment.
- `native/src/core/version_gen.h.in` comment-only bump (value from CMake).
- python `__version__` + `ENGINE_VERSION` ("0.1.0-g5").
- **Both reconfigured caches** at P5 (final phase):

```bash
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g5 native/build
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g5 native/build-asan
```

### 4.3 Diagnostics & logging deltas

| Condition | Level | Tag | Payload |
|---|---|---|---|
| Live output retarget command (SF_CMD_SET_OUTPUT) dequeued | DEBUG | runner | `live output retarget` |
| Plan recompiled with new target id | DEBUG | engine | `plan published nodes=<n>` (same as any publish) |
| Live retarget while engine has no observer | DEBUG | runner | `live output retarget` (no-op if observer null) |

---

## 5. Phases (P0…P5)

Strict main-line order; each phase leaves the tree green (ctest reg + UBSan +
pytest) and is **one commit**. Expected net-new native tests ≈ **8–12** (308 → ~316–320).
JNI grep stays **26** (no new JNI). pytest unchanged in count (version string only).

### P0 — Plan commit (Lane All, ½ day)
- **Goal:** reviewable contract — this file.
- **Files:** `docs/PLAN_G5.md`.
- **Acceptance:** §1–§9 present and internally consistent; D1–D3 explicit; all
  hardening lanes decided (A/B/decline per §3.2); no unresolved questions.
- **Validation:** human review (no code).
- **Commit:** `plan(g5): PLAN_G5.md — live engine control + G4 hardening lanes`

### P1 — `SF_CMD_SET_OUTPUT` command interception (Lane A, 1 day)
- **Goal:** runner intercepts the new command, updates the engine's target,
  triggers a plan recompile/publish.
- **Files:**
  - `native/include/soundforge/sf_command_queue.h` (EDIT — + `#define SF_CMD_SET_OUTPUT 8`).
  - `native/src/core/sf_internal.hpp` (EDIT — `RunnerObserver::set_output` callback
    pointer, internal C++ only; no public header, no export).
  - `native/src/core/command_queue_thread.cpp` (EDIT — add interception case in
    `run_loop()` that fires `observer.set_output`).
  - `native/src/audio/audio_engine.cpp` (EDIT — add `engine_set_output` callback
    and install it in `start()`).
  - `tests/unit/alloc_counter.cpp` (EDIT — G4-9 amendment: add aligned overloads
    OR comment precondition; P1 acceptance specifies choice + justification).
  - `tests/unit/CMakeLists.txt` (KEEP).
- **Acceptance:**
  - `SF_CMD_SET_OUTPUT` (cmd 8) constant defined and tested.
  - Runner intercepts the command before `apply_batch_impl`, extracts target id,
    fires `observer.set_output`; the engine-side callback updates `out_node_id`
    and re-publishes. The runner names no engine type (no `static_cast` to
    `sf_audio_engine_s` in `command_queue_thread.cpp`).
  - No new export, no new error code.
  - JNI grep stays **26**.
  - Allocator amendment: either aligned overloads compiled in, or explicit
    comment + zero-alloc assertion still holds (verified by test).
  - All G3/G4 tests still pass (no change to existing semantics).
- **Validation:** host ctest (reg + UBSan).
- **Commit:** `p1(g5): SF_CMD_SET_OUTPUT (cmd 8) — live output retarget interception`

### P2 — Memory hardening (optional, Lane B, ½ day or skip)
- **Goal:** (optional) implement G4-5 node cap or pre-size check.
- **Files:**
  - `native/src/graph/render_plan.cpp` (EDIT — optional node cap constant or check).
  - `tests/unit/test_render_plan.cpp` (EDIT — optional OOM boundary test).
- **Acceptance (if implemented):**
  - If node cap chosen: `kMaxNodes` constant defined, compile returns false +
    `SF_E_NOMEM` error when nodes exceed cap; test verifies boundary (cap-1 passes,
    cap exceeds → compile failure → `planValid:false` → silence).
  - If pre-size check chosen: same semantics, different location (check before
    pool build).
  - Justification recorded in commit message (e.g., "reduces fragmentation while
    preserving OOM semantics: last-valid plan retained").
  - All existing tests pass.
- **Validation (if implemented):** host ctest (reg + UBSan).
- **Commit (if implemented):** `p2(g5-optional): node cap hardening (G4-5) — <justification>`
- **Commit (if skipped):** (none; P2 does not land; P3 follows P1).

### P3 — Atomic<double> documentation (optional, Lane C, ¼ day or skip)
- **Goal:** (optional) document G4-6 assumption in header.
- **Files:**
  - `native/include/soundforge/sf_audio_engine.h` (EDIT — add comment block).
- **Acceptance (if implemented):**
  - Comment states: "Meter fields use `std::atomic<double>`, which is lock-free on
    aarch64/x86 (IA-64) but not guaranteed elsewhere. Generation guard (version
    counter + values) prevents torn reads."
  - No code change; comment only.
  - All tests pass.
- **Validation (if implemented):** static review (no test change).
- **Commit (if implemented):** `p3(g5-optional): atomic<double> assumption documented (G4-6)`
- **Commit (if skipped):** (none; P3 does not land; P4 or live-control test follows).

### P4 — Project-destroy hardening (optional, Lane D, ½ day or skip)
- **Goal:** (optional) implement G4-8 refuse-free guard OR document borrowing hazard.
- **Files:**
  - `native/src/core/project.cpp` (EDIT — optional check + error).
  - `native/include/soundforge/sf_audio_engine.h` (EDIT — optional borrowing
    hazard comment).
  - `tests/unit/test_audio_engine.cpp` (EDIT — optional cleanup-order test or
    confirm ordering).
- **Acceptance (if hardening chosen):**
  - `sf_project_destroy` checks `proj->audioEngine.load() != nullptr` and returns
    `SF_E_IO` + error log if true.
  - Test verifies: destroy engine first → project destroy succeeds; skip engine
    destroy → project destroy fails with `SF_E_IO`.
  - All existing tests pass.
- **Acceptance (if documented only):**
  - `sf_audio_engine.h` comment states: "Engine borrows q and p; destroy engine
    *before* project destroy. Destroying a project while an engine is attached is
    caller UB (same posture as `sf_queue_runner`)."
  - Test verifies cleanup ordering in the live-control test (destroy engine, then
    project in fixture dtor).
- **Validation:** host ctest (reg + UBSan).
- **Commit (if hardening):** `p4(g5-optional): project-destroy refuse-free guard (G4-8)`
- **Commit (if documented):** (included in P5 or skip).

### P5 — Live-control end-to-end + docs + version (Lane All, 1 day)
- **Goal:** integration test for live output change; version bump; release notes;
  DoD sweep.
- **Files:**
  - `tests/unit/test_audio_engine.cpp` (EDIT — add live-control case).
  - `native/include/soundforge/sf_version.h` (EDIT — suffix `-g5`).
  - `native/CMakeLists.txt` (EDIT — `SF_BUILD_VERSION "0.1.0-g5"`).
  - `native/src/core/version.cpp` (EDIT — fallback string).
  - `native/src/core/version_gen.h.in` (EDIT — comment-only bump to `0.1.0-g5`; value flows from CMake at configure time, mirroring G4 P6 `635de09`).
  - `python/soundforge_py/__init__.py` (EDIT — `__version__ = "0.1.0-g5"`).
  - `python/soundforge_py/migrate.py` (EDIT — `ENGINE_VERSION = "0.1.0-g5"`).
  - `docs/RELEASE_NOTES_G5.md` (NEW — gate artifact).
- **Acceptance:**
  - Live-control test: create engine with `source(-6 dB) → output`; start
    (RUNNING); tick → 0.5 amplitude; enqueue `SF_CMD_SET_OUTPUT("out")`;
    drain → next tick still 0.5 (target unchanged); enqueue
    `SF_CMD_SET_OUTPUT("src")` (new output = source); drain → next tick 1.0
    (source node output); latch advances; audit log shows no `project.migrate`,
    schemaVersion 2.
  - Full §7 suite green: ctest reg + UBSan, pytest, drift empty, grep **26**,
    `-R Version` reports `0.1.0-g5`.
  - Release notes document the live-control command, hardening decisions, and
    P0–P5 commit list.
- **Validation:** host ctest (reg + UBSan) + pytest + drift diff empty.
- **Commit:** `p5(g5): live output retarget e2e + version 0.1.0-g5 + release notes`

---

## 6. Test Plan

> Gate G5 exit: the engine supports live output retargeting via `SF_CMD_SET_OUTPUT`,
> the runner intercepts and publishes a new plan, the next tick renders the new
> target, and optional hardening lanes (G4-5/6/8/9) are implemented/documented.

### 6.1 Native unit tests (`tests/unit/`, via ctest)

| File | Notable cases |
|---|---|
| `test_cmd_queue.cpp` (EDIT — optional) | If desired: enqueue `SF_CMD_SET_OUTPUT`, dequeue, verify type/payload. Core semantics already tested by other phases. |
| `test_audio_engine.cpp` (EDIT) | **Live-control case (P5):** create engine, start (RUNNING), tick with source → out (amplitude 0.5), enqueue `SF_CMD_SET_OUTPUT("out")` (target unchanged), drain, next tick 0.5; enqueue `SF_CMD_SET_OUTPUT("src")` (retarget source), drain, next tick 1.0; latch advances; audit has no `project.migrate`; schemaVersion 2. +Optional: destroy-order test if G4-8 hardening chosen. |
| `test_render_plan.cpp` (EDIT — optional) | If G4-5 hardening chosen: node cap boundary test (cap-1 passes, cap exceeds → compile failure). |

### 6.2 Integration tests

Same as G4 (no new integration tests). The live-control case is unit-level.

### 6.3 Python tests

No new Python behavior. `pytest` count unchanged; version-string tests (if any)
update to `0.1.0-g5`.

### 6.4 Android (static-only)

- `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` = **26**
  (G5 adds **no** JNI).
- `app/**` is **KEEP** — no Kotlin, no `jni_bridge.cpp` change.

### 6.5 Verification commands (host)

```bash
cmake --build native/build -j2 && ctest --test-dir native/build --output-on-failure
cmake --build native/build-asan -j2 && ctest --test-dir native/build-asan --output-on-failure
python3 -m pytest tests/python_tests -q
diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json   # empty
grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' app/src/main/cpp/jni_bridge.cpp  # 26
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g5 native/build          # P5 reconfigure
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g5 native/build-asan
ctest --test-dir native/build -R Version                                  # sf_engine_version() == "0.1.0-g5"
```

---

## 7. Definition of Done (Gate G5)

All true on `main`:

1. **Builds:** ctest green on `native/build` (reg) **and** `native/build-asan`
   (UBSan); pytest green; JNI grep = **26**. Plan expects ≈8–12 net-new tests
   (308 → ~316–320); growth is fine when evidence-first.
2. **No drift:** `project_schema.json` byte-identical to `schema_golden_v2.json`;
   both pins untouched; `schema_golden_v1.json` untouched; schemaVersion **2**;
   no migration, no new document key, no new `SF_E_*`.
3. **Live control:** `SF_CMD_SET_OUTPUT` (cmd 8) enqueued, runner intercepts,
   updates engine's target, triggers plan recompile/publish; next tick renders
   new target. Tested.
4. **No new JNI / no Android:** `app/**` untouched; export grep 26; no Kotlin
   mirror.
5. **Version stamp `0.1.0-g5`** in `sf_version.h`, CMake default, both
   reconfigured caches, and python (`__version__`, `ENGINE_VERSION`).
6. **Hardening decisions executed:** G4-5 (cap/check/skip justified), G4-6
   (documented/stands), G4-8 (hardened/documented/stands), G4-9 (overloads
   added/comment added). All choices justified in commit messages.
7. **Docs:** this plan + `docs/RELEASE_NOTES_G5.md`; tag `g5-complete`.

---

## 8. Risks

| # | Risk | Sev | Mitigation |
|---|---|---|---|
| R1 | Live retarget command lands between doc mutation and snapshot publish; ordering hazard | MED | Runner thread owns both doc + observer; all access is single-threaded on runner lane. Sequential publish after mutation drains. Test with STOP barrier. |
| R2 | Engine `out_node_id` race if accessed outside runner thread | MED | Engine `out_node_id` is CREATED-only writable via static `set_output`, and runner-thread-only writable via the `engine_set_output` callback. No concurrent write. Read access only in the publish callback (runner thread). Runner never names the engine type. Documented. |
| R3 | Unknown command type 8 sent by old client or malformed payload | LOW | Old queue code doesn't know type 8 → default switch case in `apply_batch_impl` → `SF_E_INVALID_ARG` (safe rejection, no silent drop). New client can check sender version first if needed. |
| R4 | Hardening lanes scope ambiguity (cap vs pre-size check, hardened vs documented) | LOW | Decisions are explicit in this plan (§3.2 decision matrix); P0 review gates the choices. Each lane is isolated; choice (A/B/decline) is recorded in commit message. |
| R5 | Memory cap too tight or too loose | LOW | Node cap is advisory (OOM is already handled via `planValid:false` + last-valid retention). If cap is too tight, users simply hit it earlier (same semantics, earlier detection). If too loose, same as G4-5 (no change). |

---

## 9. Residuals & Open Questions

### 9.1 G4 residuals — disposition in G5

| # | G4 residual | Disposition in G5 |
|---|---|---|
| G4-1 | Real Android device | **STANDS** — unchanged (G5 adds no Android) |
| G4-2 | True peak not BS.1770-certified | **STANDS** — unchanged |
| G4-3 | Pacer timing best-effort | **STANDS** — unchanged |
| G4-4 | Output target fixed pre-start | **CLOSED** — G5 lands live `SF_CMD_SET_OUTPUT` command; live retargeting now supported |
| G4-5 | Plan memory ~4 KB/node | **RESIDUAL-G5-01** — optional hardening lane (node cap or pre-size check); decision at P2; if declined, G4-5 stands with same semantics |
| G4-6 | `std::atomic<double>` not portable | **RESIDUAL-G5-02** — optional documentation lane (P3); if declined, assumption stands documented in release notes |
| G4-7 | Independent two-reviewer gate | **RESOLVED** (G4 time; G5 inherits its outcome) |
| G4-8 | Project destroy cannot see engine | **RESIDUAL-G5-03** — optional hardening lane (refuse-free guard or documented borrowing); decision at P4; if declined, G4-8 stands with documented posture |
| G4-9 | Allocator alignment | **RESIDUAL-G5-04** — resolved at P1 (aligned overloads added OR comment + assertion); no G5-specific residual |
| G4-10 | Callback error paths | **STANDS** — unchanged (already specified in G4) |

### 9.2 New G5 residuals (if hardening lanes declined)

| # | Residual | Sev | Disposition |
|---|---|---|---|
| G5-1 | G4-5 (plan memory) not hardened | LOW | If declined: document in RELEASE_NOTES_G5.md that memory scales with node count; OOM is contained (last-valid plan retained, render silence). |
| G5-2 | G4-6 (atomic<double>) not documented | LOW | If declined: state in RELEASE_NOTES_G5.md that meter fields assume lock-free 64-bit atomics (aarch64/x86). |
| G5-3 | G4-8 (project destroy) not hardened | LOW | If declined: document in `sf_audio_engine.h` that engine must be destroyed before project (borrowing hazard, caller responsibility). |

### 9.3 Open questions — to be resolved at P0 gate

**Q1: Hardening lanes — implement (A), document (B), or skip?**

Answer: For each of G4-5, G4-6, G4-8, G4-9, the plan specifies options A (implement),
B (document/comment), or skip (stand as-is). **P0 plan review decides all four.** Each
decision is recorded in P1/P2/P3/P4 commit messages with justification.

---

## 10. Plan Amendments & Review Record

*(To be filled in after P0 gate. This section is reserved for oracle + security
review findings and amendments, following the G4 template. No findings yet; this
is the draft plan for submission.)*

---

*End of PLAN_G5.md*
