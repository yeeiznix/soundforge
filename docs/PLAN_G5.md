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

G5 also disposes of the four remaining G4 residuals (decided at the P0 gate,
per §3.2 D2):
- **G4-5:** Plan memory scales with node count (~4 KB/node, 4 slots → ~16 KB/node
  live). **DECLINED at gate (YAGNI):** OOM is already contained by design and
  *tested* through the `CompileFn` fault-injection seam (snapshot.hpp:78–79);
  no cap, no pre-size check. Document-only in `RELEASE_NOTES_G5.md`.
- **G4-6:** Shipped code contains **no `std::atomic<double>`** — the meter is
  plain `double m_latch[2]` (true_peak.hpp:71) serialized by ONE engine mutex
  `meter_mu` (audio_engine.cpp:98–101, SEC-G4-02(a)); torn reads are impossible.
  **RECLASSIFIED at gate:** P3 documents the real mechanism; the G4-6
  portability concern is defunct.
- **G4-8:** `sf_project_destroy` (void) cannot see an attached engine in CREATED
  state — a reachable latent UAF (engine borrows `proj`; runner never started →
  nothing guards the free). **CHOSEN at gate: implement skip-free guard** at P4,
  mirroring SEC-G3-2 (project.cpp:160–164).
- **G4-9:** `alloc_counter` replaces only default-aligned `operator new`.
  **CHOSEN at gate: add aligned overloads** at P1 (closes the zero-alloc
  assertion-evasion hole).

### 1.2 G5 MUST deliver

| Pillar | Scope | Deliverable |
|---|---|---|
| **Live output command** | `native/src/core/command_queue_thread.cpp` | New command type `SF_CMD_SET_OUTPUT` (cmd 8); runner intercepts it, extracts the target id, and **forwards it to the engine through the internal observer callback** (never touching engine internals) WITHOUT triggering `apply_batch_impl`; the engine's callback recompiles the plan with the new target and feeds the next tick |
| **Command constant** | `native/include/soundforge/sf_command_queue.h` | `#define SF_CMD_SET_OUTPUT 8` (new, public constant; no wire/schema change; command plane not `SF_E_*`) |
| **Publish without apply** | `native/src/core/command_queue_thread.cpp` | Runner intercepts `SF_CMD_SET_OUTPUT` (like `SF_CMD_STOP` and `SF_CMD_EVALUATE_MIXER`) before `apply_batch_impl` and fires the observer's **new `set_output` callback** (`user`, `cmd.id1`). The runner never dereferences the engine and never reads `sf_audio_engine_s`. |
| **Engine live-retarget** | `native/src/audio/audio_engine.cpp` (EDIT — minor) | The engine installs a second observer callback (`engine_set_output`) that writes `e->out_node_id` and re-publishes from `e->proj->doc.signalGraph` — both on the runner thread, where the doc is owned. No new export, no new field, no ABI change; `sf_audio_engine_s` stays private to its TU. |
| **Observer extension (internal)** | `native/src/core/sf_internal.hpp` (EDIT — internal C++ only) | `RunnerObserver` gains `void (*set_output)(void* user, const char* target) = nullptr;`, mirroring the existing `publish` pointer. No public header, no export, no JNI, no wire/schema change (G4 added the struct the same way). |
| **Memory hardening** | `native/src/graph/render_plan.cpp` | **DECLINED at P0 gate** (YAGNI): OOM contained by design + fault-injection-tested; document-only in release notes. No code. |
| **Meter serialization documentation** | `native/include/soundforge/sf_audio_engine.h` (P3, header comment) | Ship code has **no `std::atomic<double>`** — one engine mutex `meter_mu` serializes meter read/write (SEC-G4-02(a)). P3 comment documents this real mechanism (torn reads impossible; G4-6 portability concern defunct). |
| **Project-destroy hardening** | `native/src/core/project.cpp` (P4 — chosen) | **Skip-free guard** (NOT "return SF_E_IO" — `sf_project_destroy` is void): `if (proj->audioEngine.load(acquire) != nullptr) { log ERROR; set_handle_error; return; }`, mirroring SEC-G3-2 (project.cpp:160–164). Testable via `sf_last_error` + retry-destroy-succeeds; zero ABI change. |
| **Allocator alignment** | `tests/unit/alloc_counter.cpp` (P1 amendment) | **Chosen: aligned overloads** (`operator new/delete[/]/[]` with `std::align_val_t`) — closes zero-alloc assertion-evasion; all consumers use delta assertions, no existing test flips. |
| **Integration test** | `tests/unit/test_audio_engine.cpp` (EDIT — add live-control case) | Live output change while RUNNING, **one drain per engine session** (runner is one-shot — F2): session A `[SET_OUTPUT("out"), STOP]` → tick still targets out; session B `[SET_OUTPUT("src"), STOP]` → tick now reads src. Amplitude assertions bit-exact per F1 (see P5). No `project.migrate` entry; schemaVersion 2. |

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
  type; old clients are unaware of it. The **sync entry path** rejects it with
  the default switch case (unsupported type → `SF_E_INVALID_ARG`); a pre-G5
  runner binary drains it with an ERROR log (dropped, not silent — SEC-G5-03).
  Both are logged/signalled, never silently swallowed, and unreachable in a
  single shipped binary.

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
│   │   ├── sf_audio_engine.h               # EDIT (P3) — meter mutex-serialization comment
│   │   └── sf_version.h                    # EDIT (P5) — SF_ENGINE_VERSION_SUFFIX "-g5"
│   ├── src/dsp/
│   │   └── dsp_internal.hpp                # KEEP — unchanged (leaf invariant)
│   ├── src/graph/
│   │   ├── render_plan.cpp                 # KEEP — G4-5 declined (no cap, no pre-size)
│   │   └── CMakeLists.txt                  # KEEP
│   ├── src/audio/
│   │   └── audio_engine.cpp                # EDIT — + engine_set_output callback (runner-thread publish)
│   ├── src/core/
│   │   ├── command_queue_thread.cpp        # EDIT — intercept SF_CMD_SET_OUTPUT, fire set_output callback
│   │   ├── project.cpp                     # EDIT (P4) — G4-8 skip-free guard (mirrors SEC-G3-2)
│   │   └── sf_internal.hpp                 # EDIT — + RunnerObserver::set_output callback ptr (internal only)
│   └── src/measurement/
│       └── true_peak.hpp                   # KEEP — m_latch is plain double (no atomics)
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
| **G4-5** | Plan memory ~4 KB/node × 4 slots = ~16 KB/node; OOM contained via `planValid:false` + last-valid retention | **DECLINED at gate** | OOM is already contained by design and *tested* through the `CompileFn` fault-injection seam (snapshot.hpp:78–79); publish-failure restores the slot to EMPTY and the **previous READY plan stays current** (snapshot.hpp:16–18; F5). No real workload/fragmentation evidence in this env; a cap would refuse legal large docs for a hypothetical problem — YAGNI. Document-only in release notes. | none |
| **G4-6** | (G4's text:) `std::atomic<double>` meter fields (lock-free on aarch64/x86; not portable) | **RECLASSIFIED at gate (premise false)** | Shipped code has **no `std::atomic<double>`** — meter is plain `double m_latch[2]` (true_peak.hpp:71) serialized by ONE engine `std::mutex meter_mu` (audio_engine.cpp:98–101, SEC-G4-02(a)). No generation guard exists or is needed; the mutex already prevents torn reads. P3 = comment in `sf_audio_engine.h` documenting this real mechanism; G4-6 portability concern is **defunct** (correct the residual record in `RELEASE_NOTES_G5.md`). Do NOT ship the original "atomic lock-free plus generation guard" text — it describes something that does not exist. | P3 (documentation) |
| **G4-8** | `sf_project_destroy` cannot see an attached engine; same posture as G3 IDLE-runner hole | **CHOSEN: implement skip-free guard** | `sf_project_destroy` is `void` (project.cpp:153) — "returns `SF_E_IO`" is **unimplementable** (F4). Signature-preserving variant mirroring SEC-G3-2 (project.cpp:160–164): `if (proj->audioEngine.load(std::memory_order_acquire) != nullptr) { log ERROR; set_handle_error; return; }`. Safe: the engine's CAS claim is released in `sf_audio_engine_destroy` before the engine free (audio_engine.cpp:351–354). Testable via `sf_last_error` + retry-destroy-succeeds; zero ABI change. | P4 (hardening) |
| **G4-9** | `alloc_counter` replaces only default-aligned `operator new`; over-aligned allocations evade the zero-alloc assertion | **CHOSEN: implement aligned overloads** | Add `operator new(size_t, std::align_val_t)` + `operator delete(void*, std::align_val_t)` (+ `[]` variants) to the counter — 4 tiny functions, test-only file. All consumers use delta assertions (test_render_plan.cpp:283–295, test_true_peak.cpp:159–166) and no hot path uses over-aligned types today → no existing test flips; the zero-alloc contracts then hold against future over-aligned types. Comment-only would leave a silent hole. | P1 (amendment; tiny patch) |

### 3.3 D3 — Command interception: where to add the `SF_CMD_SET_OUTPUT` case

**Decision.** In `command_queue_thread.cpp`, **at the top of the dequeue loop in
`run_loop()`** (same place as `SF_CMD_STOP` and `SF_CMD_EVALUATE_MIXER`),
**before** the `batch[n++] = cmd;` line:

```cpp
if (cmd.type == SF_CMD_SET_OUTPUT) {
  // Control: live output retarget. Hand the new target id to the engine's
  // observer callback (opaque `user`; runner never dereferences it) so the
  // engine can update out_node_id and re-publish from the doc it owns.
  // SEC-G5-01: bound the copy — cmd.id1 is char[64] with no NUL guarantee;
  // never pass it to std::string(const char*) / strlen. strnlen keeps the
  // scan within the field: a 64-byte non-NUL payload degrades to a 64-char
  // id (unknown target -> silence), never an overread of the local struct.
  const std::string target(cmd.id1, std::strnlen(cmd.id1, sizeof(cmd.id1)));
  if (self->observer.set_output != nullptr) {
    try {
      self->observer.set_output(self->observer.user, target.c_str());
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
  - `tests/unit/alloc_counter.cpp` (EDIT — G4-9 chosen at gate: add aligned
    `operator new/delete[/]/[]` with `std::align_val_t`; see §3.2).
  - `tests/unit/CMakeLists.txt` (KEEP).
- **Acceptance:**
  - `SF_CMD_SET_OUTPUT` (cmd 8) constant defined and tested.
  - Runner intercepts the command before `apply_batch_impl`, extracts target id,
    fires `observer.set_output`; the engine-side callback updates `out_node_id`
    and re-publishes. The runner names no engine type (no `static_cast` to
    `sf_audio_engine_s` in `command_queue_thread.cpp`).
  - **SEC-G5-01:** no unbounded `strlen` / `std::string(const char*)` on
    `cmd.id1` — interception bounds via `strnlen(cmd.id1, sizeof(cmd.id1))`.
    Negative test: enqueue cmd 8 with a 64-byte `id1` (no NUL) → drained
    without overread → engine stays silent (unknown target), no crash under
    ASan-equivalent scrutiny (UBSan reg run).
  - No new export, no new error code.
  - JNI grep stays **26**.
  - Allocator amendment: aligned overloads compiled in (default-aligned + 
    `align_val_t` overloads instrumented); zero-alloc delta assertions still
    hold (all consumers use delta assertions — no existing test flips).
  - All G3/G4 tests still pass (no change to existing semantics).
- **Validation:** host ctest (reg + UBSan).
- **Commit:** `p1(g5): SF_CMD_SET_OUTPUT (cmd 8) — live output retarget interception`

### P2 — Memory hardening (Lane B) — **DECLINED at P0 gate** (no commit)
- **Gate verdict (Q1/G4-5):** OOM is already contained by design and *tested*
  through the `CompileFn` fault-injection seam (snapshot.hpp:78–79); a publish
  failure restores the slot to EMPTY and the **previous READY plan stays
  current** (snapshot.hpp:16–18), so a cap/pre-size check adds a new behavioral
  boundary (legal large docs silently refused) for a hypothetical problem —
  YAGNI. No code, no test.
- **Carry-over:** `RELEASE_NOTES_G5.md` documents that plan memory scales with
  node count (~4 KB/node live across 4 slots) and that OOM is contained
  (last-valid plan retained; never-published store renders silence). G4-5
  residual record updated accordingly (§9.1).

### P3 — Meter serialization documentation (Lane C) — reclassified at gate
- **Goal:** document the *real* meter mechanism in `sf_audio_engine.h` (G4-6
  reclassified: no `std::atomic<double>` exists in the shipped code; the meter
  is one-mutex-serialized — F3).
- **Files:**
  - `native/include/soundforge/sf_audio_engine.h` (EDIT — add comment block).
- **Acceptance:**
  - Comment states (accurate, not the G4-6 draft): "Meter fields are plain
    `double` guarded by a single engine mutex `meter_mu` that serializes
    TruePeak::process/latch/reset and meter_json/reset_meters (SEC-G4-02(a)).
    No per-field atomics, no generation guard: the mutex prevents torn reads.
    Render kernels themselves are lock-free."
  - No code change; comment only.
  - All tests pass.
- **Validation:** static review (no test change).
- **Commit:** `p3(g5): meter mutex-serialization documented (G4-6 reclassified — no atomics in codebase)`

### P4 — Project-destroy skip-free guard (Lane D) — chosen at gate
- **Goal:** close the reachable latent UAF: `sf_audio_engine_create` then
  destroy-project is not caught today (engine CREATED → runner never started →
  project freed under the engine's borrowed `proj`). Signature-preserving
  (destroy is `void`, project.cpp:153 — "return SF_E_IO" is unimplementable, F4).
- **Files:**
  - `native/src/core/project.cpp` (EDIT — add guard at top of `sf_project_destroy`,
    mirroring the SEC-G3-2 branch at lines 160–164).
  - `tests/unit/test_audio_engine.cpp` (EDIT — destroy-order test).
- **Acceptance:**
  - Guard: `if (proj->audioEngine.load(std::memory_order_acquire) != nullptr) {
      log_line(SF_LOG_ERROR, "project", "destroy: audio engine attached");
      set_handle_error(proj, "project.destroy: audio engine attached");
      return; }` — log ERROR + set handle error + **skip the free**; caller must
    `sf_audio_engine_destroy` first, then destroy again.
  - Test: (a) create engine + destroy engine first → project destroy succeeds
    (no error); (b) create engine, destroy project WITHOUT engine destroy →
    destroy returns, `sf_last_error` set, then destroy engine and destroy
    project again → succeeds. Zero ABI change.
  - All existing tests pass.
- **Validation:** host ctest (reg + UBSan).
- **Commit:** `p4(g5): project-destroy skip-free guard (G4-8) — mirrors SEC-G3-2`

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
  - Fixture: extend `set_chain` with an out-gain variant — `src(-6 dB) → out(+6 dB)`
    so the two targets differ observably. Per render semantics every node block
    has its gain applied (render_plan.cpp:152–156), so: target `out` renders
    input(1.0) × src-gain(10^(-6/20)) × out-gain(10^(+6/20)) = 0.501187 × 1.995262
    ≈ **1.0**; target `src` renders the src block **0.501187**. Both @1e-6.
  - **Live-control test (two engine sessions — the runner is one-shot, F2):**
    - *Session A (target unchanged):* start (RUNNING), tick → 1.0 (target `out`);
      drain `[SET_OUTPUT("out"), STOP]` → wait STOPPED → tick → still **1.0**
      (benign same-target retarget).
    - *Session B (retarget):* start (RUNNING), tick → 1.0; drain
      `[SET_OUTPUT("src"), STOP]` → wait STOPPED → tick → **0.501187** (source
      target, observable). Latch advances across the change; audit log shows no
      `project.migrate`; schemaVersion 2.
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
| `test_cmd_queue.cpp` (KEEP — optional) | F7 accepted at gate: optional; the intercept semantics are covered by the live-control case. Low value, skippable. |
| `test_audio_engine.cpp` (EDIT) | **Live-control case (P5, two sessions — runner is one-shot):** fixture `src(-6 dB) → out(+6 dB)`; session A: tick → 1.0 (target `out`), drain `[SET_OUTPUT("out"), STOP]`, tick → still 1.0 (benign same-target retarget); session B: tick → 1.0, drain `[SET_OUTPUT("src"), STOP]`, tick → **0.501187** (retarget observable); latch advances; audit has no `project.migrate`; schemaVersion 2. +Destroy-order test (P4): engine-destroy-first succeeds; skip-engine-destroy → guard skips free + `sf_last_error` set + retry succeeds. |
| `test_render_plan.cpp` (KEEP) | G4-5 **declined at gate** (P2 no-code) — no node-cap boundary test. OOM containment already covered via `CompileFn` fault-injection seam tests. |

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
| R3 | Unknown command type 8 sent by old client or malformed payload | LOW | Sync entry path → default switch case in `apply_batch_impl` → `SF_E_INVALID_ARG` (safe rejection); pre-G5 runner binary drains it with an ERROR log (dropped, logged, not silent — SEC-G5-03). Single shipped binary has no version skew; defensive only. |
| R4 | Hardening lanes scope ambiguity (cap vs pre-size check, hardened vs documented) | LOW | **Resolved at P0 gate** — decisions landed in §3.2/§9.3: G4-5 decline (no cap), G4-6 reclassify (mutex docs), G4-8 implement (skip-free guard), G4-9 implement (aligned overloads). |
| R5 | Memory cap too tight or too loose | LOW | **Moot** — G4-5 declined at gate; no cap. OOM remains contained via `planValid:false` + last-valid retention (never-published store renders silence). |

---

## 9. Residuals & Open Questions

### 9.1 G4 residuals — disposition in G5

| # | G4 residual | Disposition in G5 |
|---|---|---|
| G4-1 | Real Android device | **STANDS** — unchanged (G5 adds no Android) |
| G4-2 | True peak not BS.1770-certified | **STANDS** — unchanged |
| G4-3 | Pacer timing best-effort | **STANDS** — unchanged |
| G4-4 | Output target fixed pre-start | **CLOSED** — G5 lands live `SF_CMD_SET_OUTPUT` command; live retargeting now supported |
| G4-5 | Plan memory ~4 KB/node | **STANDS (declined at gate, documented)** — OOM contained by design + `CompileFn` seam-tested; no cap/pre-size (YAGNI). Release notes carry the analysis. |
| G4-6 | `std::atomic<double>` not portable | **SUPERSEDED at gate** — premise false: no `std::atomic<double>` in the codebase; meter is mutex-serialized (SEC-G4-02(a)). P3 documents the real mechanism. |
| G4-7 | Independent two-reviewer gate | **RESOLVED** (G4 time; G5 inherits its outcome) |
| G4-8 | Project destroy cannot see engine | **CLOSED** — P4 lands the skip-free guard (mirror SEC-G3-2) closing the reachable latent UAF. |
| G4-9 | Allocator alignment | **CLOSED** — P1 lands aligned overloads. |
| G4-10 | Callback error paths | **STANDS** — unchanged (already specified in G4) |

### 9.2 New G5 residuals (post-gate)

| # | Residual | Sev | Disposition |
|---|---|---|---|
| G5-1 | G4-5 (plan memory) deliberately not hardened | LOW | Documented in RELEASE_NOTES_G5.md: memory scales with node count; OOM is contained (last-valid plan retained; never-published store renders silence). |
| G5-2 | Live retarget is best-effort timing (next-tick semantics, no latency SLA) | LOW | Contract: the next rendered tick after the command drains reads the new target. No determinism/SLA added (same posture as G4-3 pacer). |
| G5-3 | `engine_set_output` re-publish on unknown target → valid plan rendering silence | LOW | SEC-G5-02: `compile_render_plan` **succeeds** with `out_index == -1` for an unknown target (render_plan.hpp:59, render_plan.cpp:102–103), so `publish` returns true and `plan_valid` stays **true** (snapshot.cpp:170) — a monitoring `meter_json.planValid` reader is not misled. `plan_valid=false` occurs only on genuine compile failure (cycle/OOM), where the last-valid plan is retained (snapshot.hpp:16–18). Same convention as static `set_output` (ORC-G4-06): unknown target is a runtime failure, not a compile failure. |

### 9.3 Open questions — resolved at P0 gate

**Q1: Hardening lanes — implement (A), document (B), or skip?**

**RESOLVED by the oracle gate (2026-09-15), recorded in §10:**

| Lane | Verdict | Justification |
|---|---|---|
| **G4-5** (plan memory) | **Decline** (document-only) | OOM already contained by design + tested through the `CompileFn` fault-injection seam; a cap adds a new behavioral boundary for a hypothetical problem (YAGNI). |
| **G4-6** (atomic\<double\>) | **Reclassify** | Premise false — no `std::atomic<double>` in the codebase; meter is mutex-serialized (SEC-G4-02(a)). P3 documents the real mechanism; residual record corrected. |
| **G4-8** (destroy vs engine) | **A — implement** (skip-free guard) | Reachable latent UAF; 3-line SEC-G3-2 mirror in a void function; testable; zero ABI change. |
| **G4-9** (allocator alignment) | **A — implement** (aligned overloads) | Closes the zero-alloc-assertion evasion; all consumers use delta assertions so no test flips. |

---

## 10. Plan Amendments & Review Record

### 10.1 P0 gate — oracle verdict: **AMEND** (2026-09-15, ses_f5a14240effeEdziVR8PFhO13w)

D1/D3 core design verified sound against source (interception site exact;
RunnerObserver field append is construction-site-safe, appended `= nullptr`;
opaque-user discipline preserved; single-writer `out_node_id` holds; publish
rollback matches `snapshot_publish`; no new export/JNI/schema). Amendments below
landed in `docs(g5): gate amendments`:

| ID | Sev | Finding | Amendment landed |
|---|---|---|---|
| **ORC-G5-01** (F1) | HIGH | Amplitudes in P5 acceptance wrong: `-6 dB → 0.5` is `10^(-6/20) = 0.501187`; retarget to `src` is NOT `1.0` — every node block has its own gain applied (render_plan.cpp:152–156), so `src` renders `0.501187` identical to `out` (retarget unobservable) | P5/§6.1 rewritten: fixture `out(+6 dB)` → `out`≈1.0 vs `src`→0.501187, both @1e-6 |
| **ORC-G5-02** (F2) | HIGH | Two STOP-barrier drains impossible — the runner is one-shot (command_queue_thread.cpp:220–224, 243–248; nothing resets STOPPED) | P5/§6.1 rewritten: **two engine sessions**, one drain each; same-target leg in session A, retarget leg in session B |
| **ORC-G5-03** (F3) | HIGH | G4-6 premise false: no `std::atomic<double>` anywhere in `native/`; meter is `double m_latch[2]` + ONE `meter_mu` mutex (SEC-G4-02(a)); no generation guard exists. The planned P3 comment would document a non-existent mechanism | P3 rewritten: comment documents mutex serialization (true_peak.hpp:71, audio_engine.cpp:98–101); G4-6 reclassified/superseded; residual record corrected |
| **ORC-G5-04** (F4) | MED | "returns `SF_E_IO`" unimplementable — `sf_project_destroy` is `void` (project.cpp:153) and §2 KEEP forbids signature change | P4 rewritten: skip-free guard mirroring SEC-G3-2 (project.cpp:160–164: log + set_handle_error + return), zero ABI change, tested via `sf_last_error` + retry |
| **ORC-G5-05** (F5) | MED | "compile failure → silence" contradicts store semantics: publish failure restores slot to EMPTY, **previous READY plan stays current** (snapshot.hpp:16–18); only never-published store or unknown target yields silence | G4-5 declined (YAGNI) so no test text remains; P2 rewritten as no-code; G5-1 residual documents retained-last-valid semantics |
| ORC-G5-06 (F6) | LOW | Version stamp surfaces verified complete/correct (CMakeLists.txt:13, sf_version.h:17, version.cpp:8, version_gen.h.in, `__init__.py:11`, migrate.py:18) | No fix — P5 keeps two-cache reconfigure |
| ORC-G5-07 (F7) | LOW | `test_cmd_queue.cpp` optional case low-value | Accepted as optional; §6.1 marks KEEP |

### 10.2 P0 gate — security reviewer verdict: **APPROVE-WITH-AMENDMENT** (2026-09-15, ses_f5a03fa76ffeXZZ9wFh1Hf7WC1)

All focus areas verified against source: command-plane interception position
matches STOP/EVALUATE exactly; observer lifetime safe (destroy race closed by
stop→join→detach→claim-release ordering); single-writer `out_node_id` + tick
never reading it → no torn plan visibility; G4-8 guard correct (acquire-load on
`std::atomic<void*> audioEngine`, claim released before engine free,
deadlock-free, mirrors SEC-G3-2); no new surface (JNI 26, drift empty, 7
SF_E_*, TU-static callback). Q1 sign-off: G4-8 APPROVE, G4-5 decline APPROVE,
G4-9 APPROVE. Amendments landed in this commit:

| ID | Sev | Finding | Amendment landed |
|---|---|---|---|
| **SEC-G5-01** | LOW | Unterminated `id1` escapes the 64-byte field as an unbounded NUL scan (`std::string(const char*)` in the callback); no crash/write corruption (adjacent `batch[]` bounds the scan), but a new consumer claims bounded handling (pre-existing exposure class in ADD_NODE) | D3 interception rewritten: `const std::string target(cmd.id1, std::strnlen(cmd.id1, sizeof(cmd.id1)))`; P1 acceptance adds the bound + a 64-byte-no-NUL negative test |
| **SEC-G5-02** | LOW | Residual G5-3 text "unknown target → silence + `plan_valid=false`" inaccurate — unknown target compiles fine with `out_index==-1`, publish returns true, `planValid:true` | G5-3 reworded: unknown target → valid plan rendering silence; `plan_valid=false` only on compile failure (last-valid plan retained) |
| **SEC-G5-03** | LOW | "No silent drop" imprecise for pre-G5 runner binaries (default branch drops queued commands with an ERROR log — logged, not silent) | §1.3 + R3 reworded: rejected with `SF_E_INVALID_ARG` on the sync path; consumed-with-ERROR-log on pre-G5 runner binaries |
| SEC-G5-04 | INFO | No log line carries the target id — no log-injection surface via `id1` | No action |

---

*End of PLAN_G5.md*
