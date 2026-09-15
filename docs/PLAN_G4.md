# SoundForge — Gate G4 Plan: Native Host-Testable Audio Engine —
# `sf_queue_runner` + `render_chain`, Per-Channel True Peak, RenderPlan Seam

> **Status: REVIEWED — planning only. No code, no commits.** This document is the
> reviewed contract for Gate G4. An adversarial review pass has run and its
> verdicts are recorded in §10.4 (R-A…R-E + B-1…B-6); R-A/R-B/R-D produced
> amendments that are **inlined** in the decisions below, and all are marked
> `(review R-x)` where they landed. **Provenance caveat:** the intended
> two-subagent gate (@oracle + @security-reviewer) could not run in this
> environment (`subagent_depth` limit = 1); §10.4 is a single-reviewer pass
> against sources at `e252dbe`. Run the independent two-reviewer gate if project
> process requires it (residual G4-7); otherwise P1 may begin after P0 lands.
> **Supersedes:** `docs/PLAN_G0.md`…`PLAN_G3.md` for G4 scope only. G0–G3 remain
> the contract for everything not changed here. G3 tag: `g3-complete`
> (`e252dbe`).
> **Deliverable:** the G3 "render harness" (`sfcore::render_chain`,
> `native/src/graph/dsp_render.cpp`) becomes a **real, host-testable native audio
> engine** (`sf_audio_engine`): a virtual device callback driven by a synthetic
> clock (and an optional pacer thread), rendering the live signal graph that
> `sf_queue_runner` mutates, with **per-channel true-peak** measurement, and the
> **preallocation seam at `dsp_render.cpp:27`** fulfilled by a compiled
> `RenderPlan` (zero allocation on the hot path).

---

## 1. G4 Scope & Non-Goals

### 1.1 Goal

G3 delivered the DSP law + finite-block kernels, the chain-render harness
(`render_chain`), and the RT queue-drain lane (`sf_queue_runner`) — but there is
still **no engine**: the harness is test-only and re-allocates per call, and
nothing measures the *rendered* signal. G4 closes that gap:

1. **Native host-testable audio engine (`sf_audio_engine`)** — binds a command
   queue + project to a renderer that consumes the *same* `sf_queue_runner` lane
   and the *same* `render_chain` kernels/law. Host-testable via `ctest`; **no
   AAudio/OpenSL**; the "device callback" is a host-side C protocol driven either
   by a deterministic synthetic clock (`sf_audio_engine_tick`) or by an optional
   pacer thread (`SF_AUDIO_ENGINE_PACE`).
2. **Per-channel true-peak measurement** — 4× oversampled polyphase FIR on the
   engine output block, per channel (L/R), held as a non-decaying latch since
   `reset_meters`; reported on a **separate JSON surface**
   (`sf_audio_engine_meter_json`) — *not* merged into the runner's
   `last_report`. This is the oracle R-A revisit mandate ("revisited at G4 when
   the render path reports per-channel true peaks", `dsp_internal.hpp:107-108`).
3. **Fulfill the preallocation seam** — `dsp_render.cpp:27` says *"G4's
   device-callback drop-in preallocates a pool outside the hot path."* G4 lands
   `RenderPlan`: compiled **at snapshot-publish time** (runner thread, outside
   the hot path) holding preallocated per-node `AudioBlock`s, topological-order
   indices, and precomputed gain/pan/active flags. The hot render path performs
   **zero allocations** (no `std::map`, no `std::set`, no `std::vector` growth).

### 1.2 G4 MUST deliver

| Pillar | Scope | Deliverable |
|---|---|---|
| **RenderPlan (seam)** | `native/src/dsp/render_plan.{hpp,cpp}` | Preallocated per-node block pool + topo indices + precomputed mixer flags; `render_chain_planned()` = zero-alloc execute; `render_chain()` re-expressed as *compile + execute* (G3 tests unchanged) |
| **True-peak meter** | `native/src/measurement/` (`sfmeasure` STATIC) | 4× oversampled polyphase FIR, per-channel K-sample lookback tail, non-decaying latch, `clipped` = latch ≥ 0 dBFS; leaf module (no locks/malloc) |
| **Plan snapshot store** | `native/src/core/snapshot.{hpp,cpp}` | Single-writer/single-reader **4-slot** lock-free handoff (writer = runner thread, reader = tick/pacer thread); no doc access from the reader; no guard exemption |
| **Runner publish hook** | `native/src/core/command_queue_thread.cpp` (+ internal `RunnerObserver`) | After each drain pass that applied ≥1 mutation, the runner (the *mutation owner*) publishes a fresh `RenderPlan` via the observer; **no new export, no JNI** |
| **Audio engine** | `native/include/soundforge/sf_audio_engine.h` + `native/src/audio/audio_engine.cpp` (`sfaudio` STATIC) | 11 host-side C exports; create/configure/set_output/start/stop/join/destroy; deterministic `tick`; optional pacer; meter JSON; `last_report` passthrough |
| **Virtual device protocol** | `sf_audio_engine_io_t` (host-side struct) | `read`/`write` callbacks per block — the host "device callback". Explicitly a host protocol, **never wired to JNI** (G2 §1.3 posture preserved) |
| **Integration** | engine + runner end-to-end | Enqueue `sf_graph_*` commands while the engine renders; the next published plan reflects the mutation; output/true-peak change accordingly |

### 1.3 G4 MUST NOT deliver (defer, leave stubs/unchanged)

- **Real Android audio device** — no AAudio/OpenSL/AAudioStream, no
  `app/src/main/cpp` change, **no new JNI** (export grep stays **26**). The
  engine is host-only, exactly like `sf_queue_runner`. `app/platform/audio/*`
  is **KEEP** (its comment already says AAudio "arrives later").
- **`float[]` across the JNI boundary** — no new `NativeBridge` function; the
  `float* const*` in the IO protocol is a host-side C struct, not JNI (D5).
- **Wire / schema change** — **schemaVersion stays 2**; `project_schema.json`,
  golden files, and both SHA-256 pins are **untouched**; no migration; no new
  document key. G4 adds *zero* wire surface.
- **New `SF_E_*` codes** — reuse `SF_E_INVALID_ARG`, `SF_E_IO`, `SF_E_NOMEM`,
  `SF_E_NOT_FOUND`. No renumbering.
- **`dspPresets[].data` authoring / DSP-effect processing** — the plan carries
  mixer gain/pan/mute/solo only; `dspPresetRef` still does not add render
  behavior (G3-4 stands).
- **Runtime output retargeting / live reconfigure** — `configure` and
  `set_output` are pre-start; changing them while RUNNING returns `SF_E_IO`.
- **Side-channel for >168 B command payloads** — G3-2 stays open.
- **Truthful RT scheduling guarantees** — the pacer is a best-effort host
  thread; no priority/affinity claims under proot (residual G4-3).

### 1.4 Gating rule

Same as G0–G3: *stub* files stay ≤20 LOC with a `// G0: stub — …` comment;
*new* files implement their bounded contract only. New G4 source files target
≤300 LOC (advisory). `sfdsp` kernels remain data-parallel leaves and are **not
modified** by G4 (leaf invariant: no locks, no malloc inside kernels).

---

## 2. File / Module Breakdown

Canonical root: `/root/project/soundforge/`. Tags: **NEW** / **EDIT** / **KEEP**.

```
soundforge/
├── native/
│   ├── CMakeLists.txt                      # EDIT — SF_BUILD_VERSION "0.1.0-g3" → "0.1.0-g4"
│   ├── include/soundforge/
│   │   ├── sf_version.h                    # EDIT — suffix "-g3" → "-g4"
│   │   └── sf_audio_engine.h               # NEW  — 11 host-side C exports + config/io structs
│   ├── src/dsp/
│   │   ├── CMakeLists.txt                  # KEEP — sfdsp leaf unchanged (kernels/law only)
│   │   └── dsp_internal.hpp                # KEEP — AudioBlock, kBlockMaxSamples unchanged (leaf invariant)
│   ├── src/graph/
│   │   ├── CMakeLists.txt                  # EDIT — + render_plan.cpp; sfgraph now compiles the plan
│   │   ├── dsp_render.cpp                  # EDIT — render_chain() = compile plan + render_chain_planned (fulfils :27 seam)
│   │   ├── graph_internal.hpp              # EDIT — render_chain_planned decl + RenderPlan fwd decl
│   │   ├── render_plan.hpp                 # NEW  — RenderPlan struct + compile/execute decls (in sfgraph, not sfdsp)
│   │   └── render_plan.cpp                 # NEW  — compile (allocates) + zero-alloc execute
│   ├── src/measurement/
│   │   ├── CMakeLists.txt                  # EDIT — INTERFACE stub → STATIC sfmeasure (target named sfmeasure, not sfmeasurement)
│   │   ├── true_peak.hpp                   # NEW  — TruePeak state + API (raw float* const* ch, not AudioBlock — leaf invariant)
│   │   └── true_peak.cpp                   # NEW  — 4× polyphase FIR + tail + latch; no sfdsp include needed
│   ├── src/audio/
│   │   ├── CMakeLists.txt                  # EDIT — INTERFACE stub → STATIC sfaudio; links sfcore + sfgraph + sfmeasure
│   │   └── audio_engine.cpp                # NEW  — sf_audio_engine implementation + observer
│   └── src/core/
│       ├── CMakeLists.txt                  # EDIT — + snapshot.cpp
│       ├── sf_internal.hpp                 # EDIT — RunnerObserver fwd decl (observer pointer on runner) + SfProject::audioEngine claim
│       ├── command_queue_thread.cpp        # EDIT — runner publishes plan after each applied batch (internal)
│       └── snapshot.hpp / snapshot.cpp     # NEW  — 4-slot plan snapshot store (internal, no export)
├── tests/
│   └── unit/
│       ├── CMakeLists.txt                  # EDIT — + 4 new test files
│       ├── alloc_counter.hpp               # NEW  — shared counting allocator (test-only; ORC-G4-01)
│       ├── alloc_counter.cpp               # NEW  — replaces global operator new/new[] once per binary
│       ├── test_dsp_render.cpp             # EDIT (declared) — INTENTIONALLY UNCHANGED; see note below (ORC-G4-02)
│       ├── test_render_plan.cpp            # NEW  — plan compile/execute, zero-alloc hot path
│       ├── test_true_peak.cpp              # NEW  — 4× oversampling, +3.01 dB, latch, reset
│       ├── test_snapshot.cpp               # NEW  — 4-slot handoff, single-writer/single-reader
│       └── test_audio_engine.cpp           # NEW  — lifecycle, tick, pacer, meter JSON, runner integration
├── python/soundforge_py/
│   ├── __init__.py                         # EDIT — __version__ "0.1.0-g3" → "0.1.0-g4"
│   └── migrate.py                           # EDIT — ENGINE_VERSION "0.1.0-g3" → "0.1.0-g4"
└── docs/
    ├── PLAN_G4.md                          # this document
    └── RELEASE_NOTES_G4.md                 # NEW — gate artifact (P6)
```

**KEEP (explicitly untouched):** `sf_types.h` (no new error codes),
`sf_command_queue.h` C ABI signatures, `sf_graph.h` signatures, `sf_project.h`
and `sf_queue_runner.h` signatures, `native/src/{acoustics,arrays,power,render}`
stubs, `native/data/schemas/project_schema.json`, `tests/golden/*`,
`python/soundforge_py/migrate.py` migration steps, `app/**` (no JNI, no Kotlin),
`native/src/dsp/kernels.cpp` / `law.cpp` / `dsp_internal.hpp`.

> **Bound check:** if a path is not listed above, do not create or edit it in G4.
> In particular `native/src/render/` stays a stub (the engine lives in
> `native/src/audio/`).

**Link-graph discipline (acyclic):**
`sfgraph → sfdsp` (unchanged); `sfmeasure → (nothing)` (leaf: depends on no
SoundForge target, includes only its own headers + `<cmath>`/`<cstdint>`);
`sfaudio → {sfcore, sfgraph, sfmeasure}`; `sfcore → sfgraph` (unchanged).
`sfgraph` does **not** link `sfmeasure` — true-peak is an *engine* concern. The
plan lives in `sfgraph` (not `sfdsp`) because `compile_render_plan` consumes
`SignalGraphDoc` and must call `topological_order`/`find_node`
(`graph_internal.hpp`): putting it in `sfdsp` would invert `sfgraph → sfdsp`
into a cycle. `sfdsp` stays a zero-dependency leaf (§2 KEEP).

---

## 3. Environment & Validation Policy

Same container as G0–G3: proot/Termux on aarch64, gcc 14.2.0, clang 21.1.8,
cmake 4.4.3, python 3.13.5, JDK 21, 7.5 GiB RAM. No Android SDK/NDK/adb.

**Validation-domain mapping** (evidence-first; statuses in capitals):

| Domain | Meaning this gate | Evidence |
|---|---|---|
| **dev** | Host editors/VMs only; Kotlin is not touched (no JNI change) | `grep` export count; static review notes |
| **native** | Host build that runs and proves: ctest (reg) + UBSan (`native/build-asan`, `-fsanitize=undefined -fno-sanitize-recover=all`) | ctest pass counts, UBSan logs |
| **native → Python** | Version strings only (no schema change) | `pytest`, drift `diff` (expect empty) |
| **integration** | Engine + runner end-to-end on host; save/reopen unaffected | `test_audio_engine.cpp` |
| **real target** (Android) | **NOT VERIFIED by design** — no SDK/NDK/adb | export-grep parity + static review only |
| **release** | Gate tag + release notes + residuals disposition | `g4-complete`, `RELEASE_NOTES_G4.md` |

Hard constraints carried from G0–G3 (unchanged):

- **ASan is dead under proot**; **UBSan is the sanctioned sanitizer**. **TSan
  remains BLOCKED** under proot (G3-8: `unexpected memory mapping`) — the new
  concurrent surfaces (snapshot handoff, pacer thread) are instead bounded by
  explicit atomics + lifecycle tests + `join`-before-destroy.
- **Additive ABI only.** No signature change to any G0–G3 export, no key
  removal, no error-code renumbering.
- **Errors via `set_last_error` / `set_handle_error` + `SF_E_*`**; no exceptions
  across the ABI (`SF_CATCH_ERRORS`); `nothrow new` → `SF_E_NOMEM`.
- **One commit per phase**, each leaving ctest (reg + UBSan) + pytest green.
- **Schema canonical == golden byte-identical** and both pins untouched (G4 adds
  no wire surface).
- **Stop-and-report** on any required-validation failure (acceptance not met →
  do not proceed to the next phase).

---

## 4. Architecture Decisions

Each decision: **Decision** / **Rationale** / **Review pointers**.

### 4.1 D1 — Read access: runner-published plan snapshots (no doc access, no guard exemption)

**Problem.** G3's single-owner rule (SEC-G3-4) makes every `doc`-touching `sf_*`
read **reject** (`SF_E_IO "project.busy: queue runner active"`) while the runner
is RUNNING/STOPPING. The engine's render tick *must* read the graph. It cannot
call `sf_graph_*` (would reject), and it cannot read `proj->doc` directly (data
race with the runner's `apply_batch_impl` mutating `nodes`/`edges` vectors).

**Decision.** The **mutation owner publishes an immutable snapshot**. After each
drain pass that applied ≥1 mutation, the runner thread compiles a
`RenderPlan` from its own `proj->doc.signalGraph` (safe: it *is* the mutator) and
publishes it through an internal observer. The engine's tick reads **only the
published plan** — no `proj` access, no lock, **no exemption** to the G3 read
guard. Before the runner starts, `sf_audio_engine_start` compiles+publishes
snapshot #0 from the caller-owned IDLE document (runner not yet live → no race).

The handoff is a **single-writer / single-reader 4-slot store** (writer = runner
thread, reader = tick/pacer thread):

```
slot state ∈ { EMPTY, WRITING, READY, READING }   (atomic per slot)
slot has: RenderPlan plan; atomic<uint64_t> seq;

PUBLISH (writer, wait-free):
  (1) scan for a slot with state EMPTY -> CAS(EMPTY, WRITING); if none,
      pick the READY slot with the SMALLEST seq -> CAS(READY, WRITING)
  (2) compile the plan INTO slot.plan (allocates; off the hot path)
  (3) SUCCESS: slot.seq = ++global_seq; state.store(READY, release)
      FAILURE: state.store(EMPTY, release)   // never publish a torn/null plan;
                                             // the previous READY plan stays current
  NOTE: the CAS in (1) can fail only if the slot changed under it (the reader
  just claimed it); the scan retries. With 4 slots the writer's target is
  always free (proof below), so publish never spins on the reader.

ACQUIRE (reader, lock-free; ≤2 slots transiently held):
  (1) find the READY slot with the LARGEST seq -> CAS(READY, READING)
  (2) on success: release the PREVIOUSLY-held slot (EMPTY, release)
  (3) no READY -> keep the plan already held (never a gap; first-ever tick
      with no published plan renders silence)
```

**Wait-free publish proof (4 slots).** At any instant the writer holds ≤1 slot
in WRITING (publish is single-threaded). The reader holds ≤1 slot at rest and
**≤2 transiently** — in ACQUIRE step (1)→(2) it claims the new slot *before*
releasing the old. Therefore the maximum simultaneously non-claimable slots is
`1 (WRITING) + 2 (READING) = 3`, leaving at least one of the 4 slots
EMPTY/READY for the writer's CAS. Hence PUBLISH never spins.

> **Amendment (review R-A):** the pre-review draft used **3** slots with
> acquire-then-release. That has a hole: 1 WRITING + 2 transient READING = all 3
> non-claimable, so the writer can spin (lock-free, not wait-free) and a hostile
> timing could stall the *runner* thread. The 4th slot removes the spin with no
> protocol change and no new correctness surface. Cost: one extra plan in
> memory (already bounded by graph size — R7/G4-5). A release-before-acquire
> 3-slot variant was rejected: it opens a one-block window with no held plan
> (an audible gap) and a reclaim race the reader must retry.

Writer targets the **oldest** READY, reader targets the **newest** READY, so a
CAS resolves contention. Memory is bounded at 4 plans; no RCU reclamation
thread, no hazard-pointer list.

**Liveness / no-permanent-loss.** If the writer publishes a newer plan between
the reader's scan and CAS, the reader acquires the older-but-valid plan for that
one tick and acquires the newer one on the next tick — eventual, never lost.
`seq` is a `uint64_t` written only by the writer; wraparound is not a practical
concern (2^64 blocks ≈ 10^7 years at 48 kHz·512) and is stated, not defended.

**Rationale.** This is the minimal design that (a) preserves SEC-G3-4 verbatim,
(b) gives the render path a lock-free, allocation-free read, and (c) keeps plan
compilation on the runner thread where allocation is already allowed.
Alternatives rejected:
- **Option B — share an internal mutex with the runner.** Rejected: puts a lock
  on the audio hot path and couples the tick cadence to mutation latency.
- **Option C — "no live edits" (freeze the graph while rendering).** Rejected:
  defeats the purpose of `sf_queue_runner`; no live graph updates.

**Review pointers (oracle + security).** (a) Prove the 4-slot protocol has no
lost-update/torn-read window and that *every* interleaving leaves a claimable
slot (the ≤1 WRITING + ≤2 transient READING ≤ 3 < 4 argument above); (b) confirm
the reader never touches `proj->doc`; (c) confirm the observer is detached before
engine destroy and the runner is joined first; (d) `seq` monotonic wraparound
(uint64 — not a practical concern, state it); (e) compile failure never publishes
a null/torn plan and never leaks a WRITING slot (it restores EMPTY).

### 4.2 D2 — Callback / clock model: synthetic `tick` + optional pacer

**Decision.** The "device callback" is a **host-side protocol struct**, not an
Android device:

```c
typedef struct sf_audio_engine_io {
  void* user;
  /* Fill the next input block. `in[c]` has `frames` floats (deinterleaved).
     Return SF_OK, or SF_E_IO to render the block as silence. May be NULL. */
  sf_result_t (*read)(void* user, float* const* in, int32_t channels, int32_t frames);
  /* Consume the rendered block. `out[c]` has `frames` floats. May be NULL. */
  sf_result_t (*write)(void* user, const float* const* out, int32_t channels, int32_t frames);
} sf_audio_engine_io_t;
```

Two drive modes, one render path:

1. **Deterministic synthetic clock** — `sf_audio_engine_tick(e, frames)`: no
   thread, no wall clock. Calls `io.read` (if set), renders `frames` through the
   current plan, calls `io.write` (if set), then updates the true-peak meters.
   This is the primary `ctest` evidence path — fully reproducible.
2. **Optional pacer thread** — `sf_audio_engine_start(e, SF_AUDIO_ENGINE_PACE)`
   spawns a host thread that repeatedly calls the same internal tick with
   `frames = max_block_frames` at a period derived from `sample_rate`. It stops
   on `stop()` and is reaped by `join()`. Timing is best-effort (residual G4-3).

`channels` is **2** (stereo) in G4; any other value → `SF_E_INVALID_ARG`.
`max_block_frames` must be `1..512` (`kBlockMaxSamples`). A single tick's
`frames` must be `1..max_block_frames`, and a tick is rejected with `SF_E_IO`
unless the engine is RUNNING.
**Defensive bound (SEC-G4-04):** `render_chain_planned` itself must reject
`block.n > kBlockMaxSamples` (return false + `err`) at its entry, and the engine
`tick` must re-validate `frames <= max_block_frames <= kBlockMaxSamples`
before touching the plan — a single off-by-one in caller validation must never
become an out-of-bounds write past the fixed 512-float per-node pool
(`render_plan.cpp:125-136` writes `j < n` into `kBlockMaxSamples` arrays).
P4 pins the `frames` bound test at BOTH edges (1 and max_block_frames).

**Rationale.** A deterministic primitive is what makes a *host* engine provable
in ctest (no flaky timing); the pacer exists only to exercise "periodic callback"
semantics. Both share one render path, so a pacer-only defect cannot hide behind
a different code path.

**Review pointers.** (a) `tick` and the pacer must not be used concurrently
(document + test); (b) pacer shutdown ordering vs runner shutdown; (c) `read`
returning `SF_E_IO` → silence, never abort; (d) no wall-clock reads inside the
deterministic path.

### 4.3 D3 — True-peak semantics: 4× polyphase FIR + tail + non-decaying latch

**Decision.** Per-channel true peak is measured on the **engine output block**
(after the plan render), 4× oversampled:

- **Interpolator:** fixed polyphase FIR, 4 phases, prototype generated by a
  documented windowed-sinc formula (compile-time constant table); unity passband
  and near-ideal reconstruction. The interpolator is **stateless per call**; the
  engine keeps a per-channel **tail** of the last `K` input samples
  (`K` = prototype half-span). Window = current block + `K`-sample lookback tail.
- **Latch:** non-decaying max of |oversampled| since `reset_meters` (or since
  create). `clipped[c]` ⇔ `latch[c] >= 1.0` (0 dBFS). True peak can exceed
  sample peak on inter-sample peaks — that is the point.
- **Reporting:** `sf_audio_engine_meter_json(e, buf, cap)` — **separate** surface
  (D6), per channel:

```jsonc
{
  "channels": 2,
  "oversample": 4,
  "blocksRendered": 128,
  "truePeakLinear": [0.7071068, 0.7071068],
  "truePeakDb":     [-3.0103,  -3.0103],   // null when the channel is silent
  "clipped":        [false, false],
  "planValid":      true                   // false after a plan compile failure
}
```

- `reset_meters` zeroes the latches (counter behavior documented: it resets the
  latch, not the rendered-frame counter).

**Meter buffer contract (SEC-G4-01, mandatory before P4):** `meter_json` must
mirror the fleet convention of `sf_queue_runner_last_report`
(`sf_queue_runner.h:74-75`, `command_queue_thread.cpp:271-273,280-284`):
- NULL `e`/`buf` or `cap == 0` → `SF_E_INVALID_ARG` (matches
  `sf_queue_runner_last_report`; do NOT overload `SF_E_NOMEM`).
- `cap > 0` but too small for the serialized payload → `SF_E_NOMEM` **and
  `buf[0] = '\0'`** — never a partial/truncated non-JSON payload.
- Serialization must be **bounded and locale-independent**: serialize into a
  fixed local buffer (fixed key set, numeric-only content), then do exactly one
  sized copy into `buf`; use C-locale/`std::to_chars`-style formatting (never
  `%g`/`%f` under a comma-decimal locale, which emits invalid JSON).
- P4 tests must assert `buf[0] == '\0'` on `SF_E_NOMEM` and the full-size
  bound (cap == exact serialized length succeeds; cap-1 → NOMEM).

**Meter synchronization layer (SEC-G4-02, mandatory before P4):**
`TruePeak` (`true_peak.hpp:32-33`, `:70-71`) is **single-thread-only by
contract** — plain `double m_latch[2]`, `float m_tail[2][24]` members. The
engine's pacer thread runs `process()` while callers may invoke
`meter_json`/`reset_meters`, so a naive direct read/write of those members
across threads is a C++ data race (UB). P4 must therefore implement **one** of
these under the engine (chosen at P4, both pinned by a concurrent test):
(a) a single engine mutex guarding `process`/`latch`/`reset` **and**
`meter_json`/`reset_meters` (tick/pacer contend only with meter reads — bounded,
no lock in the render kernels themselves), with `TruePeak` remaining
single-thread-owned; or (b) engine-owned atomics + a generation counter for the
published meter values, with **no cross-thread access to `TruePeak` members**.
Regardless of choice: `tick` vs PACE concurrency stays rejected (`SF_E_IO`),
and there must be a P4 test running PACE-style `tick` concurrently with
`meter_json`/`reset_meters` (UBSan clean) proving no data race.

**Headline acceptance:** a full-scale sine at `fs/4` with a π/4 phase offset has
sample peak `A/√2` but true peak `A` → **+3.01 dB** above sample peak. This is
the test that proves the interpolator actually oversamples rather than aliasing.

> **Numerical check (review R-C).** At `fs/4`, samples of `A·sin(2π(fs/4)t + π/4)`
> taken at integer `t` are `A·sin(nπ/2 + π/4)` = `±A·(√2/2)` — i.e. sample peak
> `A/√2` (≈ −3.01 dBFS) — while the continuous peak is `A`. A 4× phase-aligned
> interpolator places a sample at the true crest (phases 0, π/4, π/2, 3π/4 map
> onto the π/4-offset crest), so the oversampled peak reaches `A`; the +3.01 dB
> delta is therefore correct for an aligned-integer-phase layout. **P2 must
> generate the test tone at the exact grid that puts a 4× phase on the crest**;
> a tone whose crest falls between 4× grid points will under-read by the
> residual (bounded by the FIR stopband — part of the ±0.1 dB tolerance, not a
> defect). This is why the tolerance is stated, not "exact".
>
> **`clipped` threshold:** `latch >= 1.0` (0 dBFS) is the G4 contract, matching
> the G3 scalar `clipRisk` notion; it is *not* BS.1770's −1 dBTP (which is a
> meter-conformance convention, out of scope — G4-2). Documented so no consumer
> assumes −1 dBTP.
>
> **Ripple false-clip:** a full-scale in-band tone can overshoot `1.0` by the
> interpolator's passband ripple. P2 must bound the prototype's peak ripple and
> set the `clipped` test margin so a full-scale *band-limited* tone below Nyquist
> does not spuriously clip; the DC/full-scale cases pin the DC gain to unity.
>
> **NaN/Inf latch poison (review E-R-E):** the meter must not let a single
> `NaN`/`Inf` sample permanently poison the latch. Contract: compare with an
> ordered predicate (a `NaN` never updates the max) and clamp/clamp-check
> `|x|` for non-finite input; the ±1e30 test pins "stays finite".

**Rationale.** 4× oversampling is the standard true-peak approximation
(ITU-R BS.1770 class); a fixed polyphase FIR is deterministic per platform,
allocation-free, and leaf-safe (no locks/malloc — the `sfdsp` invariant extends
to `sfmeasure`). It is an *approximation*, not a certified BS.1770 meter →
residual G4-2.

**Review pointers.** (a) DC unity / passband gain (no systematic dB offset);
(b) the +3.01 dB fs/4 case within tolerance; (c) latch is monotone and reset
semantics are exact; (d) tail handling across block boundaries produces no
discontinuity (a second block of the same tone must not drop the peak);
(e) filter ripple does not produce false `clipped`.

### 4.4 D4 — `RenderPlan`: the `dsp_render.cpp:27` seam

**Decision.** `native/src/graph/render_plan.{hpp,cpp}` (compiled into `sfgraph`,
*not* `sfdsp` — it consumes `SignalGraphDoc` and calls `topological_order` /
`find_node`, so it must live on the `sfgraph → sfdsp` side of the link edge; see
§2) adds:

```cpp
namespace sfcore::dsp {

struct PlannedNode {
  AudioBlock block;      // preallocated pool (L/R arrays, size kBlockMaxSamples)
  int   kind = 0;        // SfNodeSource | Processor | Output | Bus
  float gain = 1.0f;     // 10^(gainDb/20), precomputed
  float pan = 0.0f;
  bool  pan_active = false;  // pan != 0 -> apply_pan (harness convention, G3)
  bool  excluded = false;    // mute/solo exclusion, precomputed
};

struct RenderPlan {
  std::vector<PlannedNode> nodes;            // topo order == index order
  std::vector<std::vector<int>> preds;       // predecessor topo indices
  std::vector<std::pair<std::string,int>> node_index; // id -> topo index (id map, no std::map)
  int out_index = -1;
};

// Compile at publish time (ALLOCATES — never on the hot path).
bool compile_render_plan(const SignalGraphDoc& g, const std::string& out_node_id,
                         RenderPlan& out_plan, std::string& err);

// Execute with zero allocation (kernels use explicit n).
bool render_chain_planned(const RenderPlan& plan, AudioBlock& block,
                          std::string& err);
}
```

- `compile_render_plan` performs the topology, active-set construction, and
  per-node gain/pan/exclusion computation **once**. `std::map`/`std::set` are
  gone from the hot path entirely (id→index is a flat `vector<pair<string,int>>`,
  binary-searched or sorted; `preds` is prebuilt).
- **Ordering equivalence (R-B).** The G3 harness iterates a
  `std::map<std::string, AudioBlock>` (lexicographic id order) but only ever
  *looks up* predecessors, and a node's predecessors always precede it in
  topological order, so the accumulator order is fixed by the topo walk, **not**
  by the map's key order. The plan stores nodes in topo order and looks up preds
  by index; the accumulated float sum is therefore bit-identical (same operand
  order). This is the argument P1 must pin with an explicit G3-vs-planned
  equivalence test (≤1e-12) over *every* existing `test_dsp_render` case.
  - **ORC-G4-04 (discharged P6/docs — comment-level disposition).** The
    `preds[i]` build (`render_plan.cpp:85-93`) would, for a self-loop edge
    (`fromNodeId == toNodeId`), push `i` itself as a predecessor — but that is a
    **dead corner**: `topological_order` rejects a self-loop as a cycle
    (`render_plan.cpp:25` returns `false` before `preds` is built), so
    `preds[i] == i` is unreachable. No code change; documented here.
- `render_chain_planned` walks `plan.nodes` in order, accumulates predecessors
  via `preds`, applies gate/gain/pan — the exact G3 semantics, zero allocations.
  It must not call `topological_order`, `find_node`, or allocation (asserted by
  source scan **and** an allocation-counter hook in the test).
- `render_chain(doc, block, out_id, err)` (G3, **signature unchanged**) is
  re-expressed as `compile_render_plan(...) + render_chain_planned(...)`, which
  keeps every G3 `test_dsp_render.cpp` case green while routing the engine
  through the allocation-free path.
- **Empty-graph passthrough**, **unknown-target silence + `false`**, and
  **pan==0 passthrough** semantics are preserved exactly (G3 pins them).

`PlannedNode`s are sized to the graph at compile time; a plan for `N` nodes holds
`N × sizeof(AudioBlock)` (~4 KB each) — dominated by the fixed 512-sample arrays,
which is the price of a genuinely allocation-free hot path. If allocation fails,
compile returns `false` with `SF_E_NOMEM`-class error and the engine publishes a
**null plan** (renders silence, sets an observable meter/status flag) rather than
stalling the runner.

> **Null-plan / OOM policy (amended, review R-B).** "Publish a null plan" means
> the **snapshot store restores the slot to `EMPTY` and keeps the last valid plan
> current** — it does **not** overwrite a READY slot with a null plan (that would
> destroy the last good audio). The engine exposes a `planCompileFailed` flag
> (atomic, surfaced in `meter_json` as `"planValid": false`) and logs the §5.4
> ERROR row. The audio thread keeps rendering the previous valid plan; it never
> stalls and never allocates from the failure path.

**Rationale.** This is literally what `dsp_render.cpp:27` asked for. It also
removes a per-call `std::map` allocation from a path that is now driven by a
device callback.

**Review pointers.** (a) G3 `render_chain` tests pass **unmodified**; (b) a
`grep`/test proves `render_chain_planned` contains no allocating call
(`new`/`std::map`/`push_back`); (c) compile failure path is safe (silence, not a
crash, not a stall); (d) gain/pan/exclusion values are byte-identical to the G3
computation for the same graph.

### 4.5 D5 — ABI surface: `sf_audio_engine.h`, 11 host-side exports, no JNI

**Decision.** New host-only public header, no JNI, no `NativeBridge` mirror.
Exports (11):

```c
typedef struct sf_audio_engine_s sf_audio_engine_t; /* opaque */

typedef struct sf_audio_engine_config {
  int32_t sample_rate;        /* > 0; e.g. 48000 */
  int32_t channels;           /* must be 2 (G4) */
  int32_t max_block_frames;   /* 1..512 (kBlockMaxSamples) */
  sf_audio_engine_io_t io;    /* may be zero-initialized (NULL callbacks) */
} sf_audio_engine_config_t;

#define SF_AUDIO_ENGINE_PACE 0x1u

sf_result_t sf_audio_engine_create(sf_audio_engine_t** out, sf_cmd_queue_t* q,
                                   sf_project_t* p);              /* 1 */
sf_result_t sf_audio_engine_destroy(sf_audio_engine_t* e);        /* 2 */
sf_result_t sf_audio_engine_configure(sf_audio_engine_t* e,
                                      const sf_audio_engine_config_t* cfg); /* 3 */
sf_result_t sf_audio_engine_set_output(sf_audio_engine_t* e,
                                       const char* out_node_id);  /* 4 */
sf_result_t sf_audio_engine_start(sf_audio_engine_t* e, uint32_t flags); /* 5 */
sf_result_t sf_audio_engine_stop(sf_audio_engine_t* e);           /* 6 */
sf_result_t sf_audio_engine_join(sf_audio_engine_t* e);           /* 7 */
sf_result_t sf_audio_engine_tick(sf_audio_engine_t* e, size_t frames); /* 8 */
sf_result_t sf_audio_engine_last_report(const sf_audio_engine_t* e,
                                        char* buf, size_t cap);   /* 9 */
sf_result_t sf_audio_engine_meter_json(const sf_audio_engine_t* e,
                                       char* buf, size_t cap);    /* 10 */
sf_result_t sf_audio_engine_reset_meters(sf_audio_engine_t* e);   /* 11 */
```

**Contract summary (mirrors `sf_queue_runner`, one-shot lifecycle):**

- `create` — requires `p.runnerState == IDLE`; **atomically claims the
  single-engine slot** via an internal `SfProject::audioEngine`
  `std::atomic<sf_audio_engine_t*>` CAS(nullptr → e) (new internal field, **no
  new export, no new `SF_E_*`**) and then creates its internal `sf_queue_runner`
  on `(q, p)` (the engine owns it). On any failure the claim is rolled back and
  the runner torn down — no half-bound state. `SF_E_IO` if an engine (or, at
  start, a runner) is already attached.
  > **Amendment (review R-D):** the draft relied on `sf_queue_runner_create`
  > rejecting a busy project, but that call only *checks* `runnerState == IDLE`
  > and does **not** reserve it (`command_queue_thread.cpp:199-203`). Two engines
  > could both pass `create`. The internal `audioEngine` CAS is the real
  > single-engine guard; `runnerState` remains owned by the runner lifecycle
  > exactly as in G3.
- `configure` / `set_output` — **CREATED only** (pre-start); `SF_E_IO` otherwise.
  A NULL `out_node_id`/`""` selects "no output" (renders silence).
- `start` — **ordered gate + snapshot #0 + rollback (SEC-G4-05):** (1) first
  acquire-load `p.runnerState` and reject (`SF_E_IO`) if a runner is already
  live (`runner_thread_alive(...)` or `runnerState != IDLE`) **before** reading
  `proj->doc` or compiling snapshot #0 — a standalone runner could otherwise be
  created in the create→start window because `sf_queue_runner_create` only
  *checks* `runnerState` without reserving it (SEC-G4-05, R-D);
  (2) **compiles+publishes snapshot #0 from the IDLE document** (runner not yet
  RUNNING → caller-owned doc readable, no guard violated); (3) starts the
  internal runner, then optionally the pacer; **(4) rollback:** if the internal
  runner start (or pacer spawn) fails after snapshot #0 was published, stop+join
  what was started, restore the slot/store to the pre-start state (snapshot #0
  unpublished or marked stale), return `SF_E_IO`, and leave the engine CREATED
  (not RUNNING) so `destroy` still works. One-shot: from RUNNING → `SF_E_IO
  "audio engine: already started"`.
- `tick` — RUNNING only; the deterministic render primitive. `tick` and the
  pacer must never run concurrently: `start(PACE)` sets a flag and `tick`
  returns `SF_E_IO` while PACE is active (documented + tested).
- `stop`/`join`/`destroy` — mirror the runner (STOPPING/STOPPED, idempotent,
  destroy requires not-RUNNING **and not STOPPING** — STOPPING means the thread
  is still alive and `sf_queue_runner_destroy` would return `SF_E_IO`).
  **NULL/double-destroy (SEC-G4-10):** `destroy(NULL)` → `SF_OK` no-op;
  double-destroy of a live handle = caller UB (documented one-shot, same as the
  runner — `sf_queue_runner.h:80-85`); the `audioEngine` claim release
  (CAS e → nullptr) runs **before/independently of freeing the engine** so a
  failed-create rollback can never leave the claim set.
- **Destroy ordering (R-D, must be exact):** `join pacer thread` → `runner.stop()`
  → `runner.join()` → **detach observer (set the runner's observer pointer null)**
  → `sf_queue_runner_destroy(internal)` → free snapshot store → free engine →
  release the `audioEngine` claim (CAS e → nullptr). Detaching the observer
  *after* the runner join means no publish can be in flight when the engine is
  freed, so the observer pointer can never dangle.
- **Caller-lifetime hazard (R-D).** The engine borrows `q` and `p` like the
  runner does (`command_queue_thread.cpp:105-106`); both must outlive the engine.
  This is documented in the header, and `destroy` must be called before the
  caller destroys `p`. (An engine that outlives its project is caller UB, same
  posture as `sf_queue_runner`; G4 does not add a registry to detect it.)
- `last_report` — **passthrough** to `sf_queue_runner_last_report` (D6).
- `meter_json` / `reset_meters` — meter surface (D3); safe in any state.

**Engine state machine:** `{CREATED, RUNNING, STOPPING, STOPPED}` (atomic), with
RUNNING→STOPPED written only by the join/epilogue path (same discipline as
SEC-G3-3).

**Coupling between engine states and runner states** (must be documented and
tested): engine RUNNING ⇔ runner RUNNING/STOPPING; the G3 read/mutator guards
therefore apply to the project for the engine's RUNNING lifetime, which is
expected (the engine *is* the runner's owner). Engine `tick`/`meter_json`/
`last_report` are the only calls valid while RUNNING.

**Rationale.** A host-only C surface keeps the "no JNI, no `float[]` across
JNI" posture (G2 §1.3). The `float* const*` in the IO struct is a host device
protocol — it never crosses the Kotlin boundary because no JNI function exposes
it. Reusing the runner lifecycle verbatim minimizes new concurrency surface.

**Review pointers (security).** (a) `create` must not leave a half-bound runner
on failure; (b) destroy ordering (pacer → runner → observer detach → free);
(c) the engine must return an error, never crash, on misuse (tick before start,
configure after start, double start); (d) confirm no new JNI symbol (grep 26).

### 4.6 D6 — `last_report` / `EVALUATE_MIXER`: **no merge**

**Decision.** The desk report (runner `EVALUATE_MIXER`, cmd 7) stays exactly as
shipped. `sf_audio_engine_last_report` is a **thin passthrough** to
`sf_queue_runner_last_report`; true peaks are reported on the **separate**
`sf_audio_engine_meter_json` surface. They are **not** merged.

**Rationale.** (a) Different natures: the desk report is a *static routing
estimate* (scalar coefficients, per-output), true peak is a *measured rendered
signal* (per-channel, time-dependent). (b) Different lifetimes: the desk report
is single-slot overwrite-on-evaluate; true peak is a monotone latch. (c) Merging
would mutate the shipped G3 JSON shape and force the runner to know about
meters. Keeping them separate preserves every G2/G3 JSON consumer and makes both
independently testable.

**Review pointers.** (a) `last_report` must remain byte-identical to G3 for the
same evaluate; (b) `meter_json` must not appear inside `last_report` (asserted);
(c) both surfaces must tolerate a not-yet-populated state (`SF_E_IO` for
`last_report`, `truePeakDb: null`/`clipped:false` for the meter).

---

## 5. Schema, Wire & Version Impact

### 5.1 Wire delta — **none**

G4 adds **no** document key, **no** command type, **no** error code, and **no**
migration. `schemaVersion` stays **2**; `project_schema.json` byte-identical to
`schema_golden_v2.json`; both SHA-256 pins untouched; `schema_golden_v1.json`
untouched. The drift `diff` in §7.5 must remain empty. This is a deliberate,
review-visible posture: G4 is a pure native/engine gate.

### 5.2 Golden & fixture discipline

No fixture changes. The existing `tests/fixtures/*` continue to load. New engine
tests build graphs **in memory** (no new fixture files) or reuse
`project_dspchain_v2.json` read-only.

### 5.3 Version stamp

G4 lands **`0.1.0-g4`** (suffix-tag convention, G3 §5.3 precedent):
`sf_version.h` suffix (`SF_ENGINE_VERSION_SUFFIX`, the macro `SF_ENGINE_VERSION`
derives), `native/CMakeLists.txt` default, python `__version__` +
`ENGINE_VERSION`, and **both reconfigured caches**:

```bash
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g4 native/build
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g4 native/build-asan
```

The non-CMake fallback (`native/src/core/version.cpp`) and the build-stamp
template comment (`native/src/core/version_gen.h.in`) are bumped in lockstep, as
in G3; `app/src/main/cpp/CMakeLists.txt` is **KEEP** (Android, `-g0` historical,
`app/**` untouched). `SF_SCHEMA_VERSION` stays **2**; `sf_is_compatible`
unchanged. *(Resolved in §10.3 Q1: the bump is in scope — suffix-only, no wire
change.)*

### 5.4 Diagnostics & logging deltas (existing `sf_log` ring)

| Condition | Level | Tag | Payload |
|---|---|---|---|
| Engine start/stop | `INFO` | `engine` | `engine start/stop handle=<h>` |
| Engine tick misuse (not RUNNING) | `WARN` | `engine` | `tick: engine not running` |
| Plan publish (mutation applied) | `DEBUG` | `engine` | `plan published nodes=<n>` |
| Plan compile failure | `ERROR` | `engine` | `plan compile failed: <err>` |
| Pacer spawn failure | `ERROR` | `engine` | `pacer: thread spawn failed` |

---

## 6. Phases (P0…P6)

Strict main-line order; each phase leaves the tree green (ctest reg + UBSan +
pytest) and is **one commit**. Expected net-new native tests ≈ **45** (232 →
~277). JNI grep stays **26** (no new JNI). pytest unchanged in count (version
string only).

### P0 — Plan commit (Lane All, ½ day)
- **Goal:** reviewable contract — this file.
- **Files:** `docs/PLAN_G4.md`.
- **Acceptance:** §1–§10 present and internally consistent; D1–D6 explicit with
  review amendments inlined; adversarial review **done** and recorded in §10.4;
  no unresolved HIGH finding remains.
- **Validation:** human review (no code).
- **Commit:** `plan(g4): PLAN_G4.md — reviewed native audio engine + true peak + RenderPlan seam`

### P1 — `RenderPlan` + zero-alloc `render_chain_planned` (Lane A, 1 day)
- **Goal:** fulfil the `dsp_render.cpp:27` seam; G3 harness semantics preserved.
- **Files:** `native/src/graph/render_plan.hpp`, `render_plan.cpp` (NEW),
  `native/src/graph/CMakeLists.txt` (EDIT — + render_plan.cpp),
  `native/src/graph/dsp_render.cpp` (EDIT — `render_chain` = compile + planned),
  `native/src/graph/graph_internal.hpp` (EDIT — decl),
  `tests/unit/test_render_plan.cpp` (NEW), `tests/unit/alloc_counter.hpp`,
  `tests/unit/alloc_counter.cpp` (NEW — shared counting allocator; ORC-G4-01),
  `tests/unit/test_dsp_render.cpp` (EDIT — *declared*; **finally untouched**,
  see note), `tests/unit/CMakeLists.txt` (EDIT).
- **Note (ORC-G4-02, discharged P6/docs):** `test_dsp_render.cpp` was declared
  EDIT for per-case planned-path equivalence, but landed **unchanged** — the
  equivalence seam is proven instead through the facade: `dsp_render.cpp:40-45`
  (`render_chain` = `compile_render_plan` + `render_chain_planned`, unmodified
  G3 signature/semantics) runs every existing G3 `Render.*` case through the
  planned path, and `test_render_plan.cpp` adds planned value tests plus the
  structural operand-order identity (D4/preds accumulation in edge document
  order). The planned path is exercised for every G3 case by construction.
- **Acceptance:** every existing G3 `test_dsp_render` case passes **unmodified**;
  `render_chain_planned` produces identical output to G3 `render_chain` for the
  same graph (relative tolerance ≤1e-12) — the operand-order argument in D4 must
  hold for every case; 2-node −6 dB DC → half amplitude; mute/solo exclusion;
  empty graph → identity; unknown target → silence + `false`; pan==0 passthrough;
  the hot execute path proves no allocation both by source scan (`std::map`/`new`
  absent from the execute loop) **and** an allocation-counter/`malloc`-hook;
  compile failure (OOM simulation / cycle) restores EMPTY and leaves the
  last valid plan current (no null overwrite, no WRITING leak). Unknown target
  is NOT a compile failure (D4: compiles with out_index=-1) — it is a
  render-failure (silence + `false`); only genuine compile failures (cycle /
  OOM / exception) exercise the EMPTY-restore path.
- **Validation:** host ctest (reg + UBSan).
- **Commit:** `p1(g4): RenderPlan preallocated pool + render_chain_planned (dsp_render seam)`

### P2 — True-peak meter module (Lane B, 1–1.5 days)
- **Goal:** per-channel 4× oversampled true peak, host-unit-testable, leaf-safe.
- **Files:** `native/src/measurement/CMakeLists.txt` (EDIT — INTERFACE →
  STATIC **`sfmeasure`**; the shipped stub target is named `sfmeasurement`, so
  the rename is explicit and the top-level `add_subdirectory` loop keeps working),
  `true_peak.hpp`, `true_peak.cpp` (NEW),
  `tests/unit/test_true_peak.cpp` (NEW), `tests/unit/CMakeLists.txt` (EDIT).
- **Acceptance:** fs/4 + π/4 sine → true peak **+3.01 dB** (±0.1 dB) above
  sample peak; DC unity (±1e-4) — no systematic offset; two consecutive blocks
  of the same tone hold the peak (tail correctness, no boundary discontinuity);
  latch monotone (never decreases until reset); `reset` zeroes; silence →
  linear 0 / dB `null`, `clipped=false`; full-scale DC / fs4 sine → `clipped`
  true where expected; determinism (two identical runs equal ≤1e-12);
  no NaN/Inf for ±1e30 input; module has no locks/malloc in the process path.
- **Validation:** host ctest (reg + UBSan).
- **Commit:** `p2(g4): sfmeasure true-peak — 4x polyphase FIR + latch`

### P3 — Snapshot store + runner publish hook (Lane C, 1–1.5 days)
- **Goal:** runner-published, lock-free single-reader plan handoff.
- **Files:** `native/src/core/snapshot.hpp`, `snapshot.cpp` (NEW),
  `native/src/core/CMakeLists.txt` (EDIT), `native/src/core/sf_internal.hpp`
  (EDIT — internal `RunnerObserver` decl + `SfProject::audioEngine` claim),
  `native/src/core/command_queue_thread.cpp`
  (EDIT — publish after each applied batch), `tests/unit/test_snapshot.cpp`
  (NEW), `tests/unit/CMakeLists.txt` (EDIT).
- **Acceptance:** 4-slot protocol unit tests: publish when all slots EMPTY;
  publish when no EMPTY slot remains (oldest READY reclaimed); reader acquires
  newest READY and keeps it across "ticks"; release-then-reclaim; **stress** loop
  (writer publishes N plans while reader repeatedly acquires) never returns a
  torn/partial plan, never loses the newest published plan, and **the writer
  never spins** (asserted — the 4th-slot guarantee); reader with no READY keeps
  its previous plan; compile-failure restores the slot to EMPTY and leaves the
  last valid plan current; `seq` monotone. Runner integration: enqueue a mutation
  while the runner is started → observer fires after the apply → a new plan is
  READY with the mutated graph; an empty drain does **not** republish;
  `SF_CMD_STOP`/cmd 7 do not republish. Observer detached cleanly before destroy;
  runner never writes `proj->lastError` (SEC-G3-4 unchanged).
- **Validation:** host ctest (reg + UBSan); TSan attempt recorded (expected
  BLOCKED → G3-8).
- **Commit:** `p3(g4): plan snapshot store + runner publish hook`

### P4 — `sf_audio_engine` module (Lane D, 2 days)
- **Goal:** the host audio engine: lifecycle, deterministic tick, optional
  pacer, meter JSON, `last_report` passthrough.
- **Files:** `native/include/soundforge/sf_audio_engine.h` (NEW),
  `native/src/audio/CMakeLists.txt` (EDIT — INTERFACE → STATIC `sfaudio`;
  links `sfcore` + `sfgraph` + `sfmeasure`),
  `native/src/audio/audio_engine.cpp` (NEW),
  `tests/unit/test_audio_engine.cpp` (NEW), `tests/unit/CMakeLists.txt` (EDIT).
- **Acceptance:** lifecycle create→configure→set_output→start→tick→stop→join→
  destroy; create requires IDLE and **claims the internal `audioEngine` slot
  atomically** (second create → `SF_E_IO`, no half-bound runner left on failure);
  configure/set_output reject post-start (`SF_E_IO`); tick before start →
  `SF_E_IO`; `frames` bounds; channels≠2 → `SF_E_INVALID_ARG`; deterministic tick
  renders a known graph (input DC → output scale matches the law) and the meter
  reports the expected peak; `reset_meters` zeroes; `meter_json` is valid JSON
  with the documented keys and `null` dB when silent; `meter_json` NULL `e`/`buf`
  or `cap==0` → `SF_E_INVALID_ARG`, too-small `cap>0` → `SF_E_NOMEM` **and
  `buf[0]='\0'`** (never writes past the buffer, never truncated payload; null
  termination asserted — SEC-G4-01 contract in §4.3); `last_report` passthrough returns the runner's report
  byte-identically and `SF_E_IO` before any evaluate; PACE start spawns a pacer
  that advances `blocksRendered` and stop/join reaps it within bounded time (no
  deadlock); `tick` while PACE is active → `SF_E_IO`; destroy requires
  not-RUNNING **and not STOPPING**, auto-joins the pacer then the runner, detaches
  the observer, and only then frees (ordering asserted under UBSan); JNI grep
  stays 26; UBSan clean.
- **Validation:** host ctest (reg + UBSan); Android `NOT VERIFIED` by design
  (no SDK/NDK — G2-4). **Commit:**
  `p4(g4): sf_audio_engine — host device callback + pacer + meter json (no new jni)`

### P5 — Engine + runner end-to-end (Lane D, 1 day)
- **Goal:** prove the live loop: mutate via the queue, hear it in the next
  rendered block, measure it.
- **Files:** `tests/unit/test_audio_engine.cpp` (EDIT — add end-to-end cases),
  `tests/unit/test_dsp_render.cpp` (EDIT — only if the equivalence seam needs a
  G3 case), possibly `tests/integration/CMakeLists.txt` (EDIT) if the end-to-end
  case is placed in `tests/integration/test_audio_engine_integration.cpp` (NEW).
- **Acceptance:** start engine (no PACE) with a graph `source(-6 dB) → output`;
  `tick` with a DC input → output = half amplitude, meter true peak = 0.5; enqueue
  `SF_CMD_SET_MIXER(gain=0 dB)` → drain → next `tick` renders 1.0 amplitude and
  the true-peak latch advances to 1.0 (`clipped` true); a mutation that mutes the
  output → next tick renders silence while the latch **holds** the previous peak
  (non-decaying); external doc-touching `sf_graph_*` read still rejects
  `SF_E_IO "project.busy: queue runner active"` while the engine runs; after
  stop+join the handle is usable synchronously again (G3 C8); no
  `project.migrate`; schemaVersion 2; UBSan clean.
- **Validation:** host ctest (reg + UBSan) + pytest + drift diff empty.
- **Commit:** `p5(g4): audio engine + queue runner end-to-end; true-peak hold across mutations`

### P6 — Docs + DoD sweep + tag (Lane All, ½ day)
- **Goal:** gate artifact.
- **Files:** `docs/RELEASE_NOTES_G4.md` (NEW), this plan finalized
  (§10.4 verdicts recorded + finding-discharge table added), version bump
  `0.1.0-g4` applied + both caches reconfigured.
- **Acceptance:** full §7.5 suite green: ctest reg + UBSan, pytest, drift empty,
  grep **26**, `-R Version` reports `0.1.0-g4`; §10.4 verdict table every
  finding discharged or re-deferred with a residual (G4-7 records the
  independent-reviewer process item); `docs/RELEASE_NOTES_G4.md` records the
  evidence table + ORC-G4-02/05 comparison-pair limitation + version-stamp
  surface; tag `g4-complete`.
- **Validation:** evidence table in release notes (G3 style). **Commit:**
  `p6(g4): release notes + DoD sweep; version 0.1.0-g4` (no tag — the
  orchestrator tags `g4-complete` after the independent final audit).

---

## 7. Test Plan

> Gate G4 exit: the engine renders live-mutated graphs through a preallocated
> plan with zero hot-path allocation, host-proven per-channel true peak, and the
> G0–G3 suites still green.

### 7.1 Native unit tests (`tests/unit/`, via ctest)

| File | Notable cases |
|---|---|
| `test_render_plan.cpp` (NEW, ≈10) | compile topo order; per-node gain/pan/exclusion values match G3 computation; planned execute == G3 `render_chain` (≤1e-12); DC −6 dB → half amplitude; mute/solo exclusion; empty graph identity; unknown target silence+false; pan==0 passthrough; **no-allocation** assertion on the execute path (source scan + a counting-allocator or `malloc`-hook); compile-failure path safe. **ORC-G4-05 (discharged P6/docs):** the `ExecuteMatchesG3RenderChainBitForBit` test compares the *planned* path against the `render_chain` facade — and since the facade is now literally `compile_render_plan` + `render_chain_planned` (`dsp_render.cpp:40-45`), it is the same path twice. It is retained as the G3-contract regression (the facade is what G3 callers use); the real cross-path proof is the 8 untouched `test_dsp_render` `Render.*` G3 cases (bit-exact G3 expectations vs the facade) plus the direct planned value tests in this file. |
| `test_true_peak.cpp` (NEW, ≈12) | fs/4 + π/4 sine → +3.01 dB ±0.1; DC unity ±1e-4; multi-block hold (tail); latch monotone; reset → 0; silence → 0/`null`/not-clipped; full-scale → clipped; ±1e30 finite; determinism ≤1e-12; per-channel independence (L silent, R loud); block-size invariance of the *latch* (512×1 vs 64×8). |
| `test_snapshot.cpp` (NEW, ≈11) | publish into EMPTY; reclaim oldest READY; reader acquires newest; reader keeps plan when no READY; release/reclaim; stress writer vs reader (no torn plan, newest not lost, **writer never spins**); `seq` monotone; compile-failure restores EMPTY (last valid plan retained); 4-slot bound holds under many publishes. |
| `test_audio_engine.cpp` (NEW, ≈20) | lifecycle + one-shot + misuse errors; create-requires-IDLE; configure/set_output pre-start only; tick bounds + not-running reject; deterministic render + meter; `meter_json` schema/keys/`null`; `reset_meters`; `last_report` passthrough + before-evaluate `SF_E_IO`; PACE pacer advances + stop/join bounded; destroy auto-join; engine+runner end-to-end mutations (P5): gain change, mute holds latch, read-guard still rejects, post-join sync use; JNI grep 26; UBSan clean. |
| `test_dsp_render.cpp` (EDIT) | G3 cases unchanged; +planned-path equivalence for each existing case (prove the seam is behavior-preserving). |

### 7.2 Integration tests (`tests/integration/`)

| Test | Steps | Pass |
|---|---|---|
| `test_project_io.cpp` (KEEP unless a version-string assertion exists) | save/reopen unaffected by G4 | no migration; schemaVersion 2 |

*(If the end-to-end engine+runner case is placed in `tests/integration/`, the file
is `test_audio_engine_integration.cpp` (NEW) with the P5 acceptance cases;
otherwise it lives in `test_audio_engine.cpp`. The plan permits either; P5
records the choice.)*

### 7.3 Python tests

No new Python behavior. `pytest` count unchanged; version-string tests (if any
assert `__version__`) update to `0.1.0-g4`.

### 7.4 Android (static-only — no SDK/NDK, no `assembleDebug`, no logcat)

- `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` = **26**
  (G4 adds **no** JNI).
- `app/**` is **KEEP** — no Kotlin and no `jni_bridge.cpp` change. The engine is
  a host-only native surface; the Android device lane remains `NOT VERIFIED`
  (G2-4).

### 7.5 Verification commands (host)

```bash
cmake --build native/build -j2 && ctest --test-dir native/build --output-on-failure
cmake --build native/build-asan -j2 && ctest --test-dir native/build-asan --output-on-failure
python3 -m pytest tests/python_tests -q
diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json   # nothing
grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' app/src/main/cpp/jni_bridge.cpp  # 26
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g4 native/build          # P6 reconfigure (both dirs)
ctest --test-dir native/build -R Version                                  # sf_engine_version() == "0.1.0-g4"
```

---

## 8. Definition of Done (Gate G4)

All true on `main`:

1. **Builds:** ctest green on `native/build` (reg) **and** `native/build-asan`
   (UBSan substitute); pytest green; JNI export grep = **26**. Plan expects ≈45
   net-new native tests (232 → ~277); growth beyond plan is fine when
   evidence-first.
2. **No drift:** `project_schema.json` byte-identical to `schema_golden_v2.json`;
   both pins untouched; `schema_golden_v1.json` untouched; schemaVersion **2**;
   no migration, no new document key, no new `SF_E_*`.
3. **RenderPlan seam:** `render_chain(doc,…)` unchanged in signature and
   behavior; `render_chain_planned` is allocation-free on the execute path
   (asserted); `dsp_render.cpp:27`'s preallocation promise is fulfilled.
4. **True peak:** per channel, 4× oversampled, fs/4 +3.01 dB proven, latch
   non-decaying with exact reset, `clipped` = latch ≥ 0 dBFS, reported on the
   separate `meter_json` surface.
5. **Snapshot:** single-writer/single-reader **4-slot** lock-free handoff; runner
   publishes after each applied mutation; tick never touches `proj->doc`; no
   exemption to the G3 read guard; stress-tested.
6. **Engine:** 11 host-side exports; deterministic tick + optional pacer;
   lifecycle one-shot with misuse errors; `last_report` passthrough byte-identical
   to G3; plan-compile failure is safe (silence, not stall/crash).
7. **No new JNI / no Android:** `app/**` untouched; export grep 26; the
   `float* const*` IO struct is a host protocol, never exposed to Kotlin.
8. **Version stamp `0.1.0-g4`** in `sf_version.h`, CMake default, both
   reconfigured caches, and python (`__version__`, `ENGINE_VERSION`).
9. **Concurrency:** pacer/runner shutdown ordering (pacer → runner → observer
   detach → free) tested; no deadlock; UBSan clean; TSan attempt recorded
   (BLOCKED → G3-8).
10. **Docs:** this plan + `docs/RELEASE_NOTES_G4.md`; §10 dispositions resolved;
    tag `g4-complete`.

**Exit artifact:** tag `g4-complete`; release notes attach the engine ABI
summary, the true-peak spec, the snapshot protocol description, and the §10
residuals table.

---

## 9. Risks

| # | Risk | Sev | Mitigation |
|---|---|---|---|
| R1 | Snapshot protocol torn/lost-update race (the gate's highest-risk surface) | **HIGH** | Airtight **4-slot** single-writer/single-reader protocol (D1, amended after review R-A) + stress test (§7.1); oracle+security review pointer; TSan requested (expected BLOCKED, G3-8) |
| R2 | `render_chain` behavior drifts when re-expressed through the plan | MED | G3 tests pass unmodified + explicit planned/G3 equivalence cases (P1) |
| R3 | True-peak approximation is not BS.1770-certified; false clipped/underread | MED | Documented approximation (G4-2); DC-unity + fs/4 +3.01 dB + multi-block hold tests; tolerances stated |
| R4 | Pacer thread timing/races under proot (no RT scheduling) | MED (env) | Best-effort pacer only; deterministic tick is the primary evidence; bounded stop/join test |
| R5 | Engine ↔ runner lifecycle coupling leaves the handle unusable after misuse | MED | One-shot states mirror the runner; explicit misuse error tests; P5 proves post-join sync use (C8) |
| R6 | ASan/TSan remain blocked under proot | MED (env) | UBSan + explicit atomics + lifecycle/reap tests (G3-8 carried) |
| R7 | Plan memory grows with node count (4 KB/node pool) | LOW | Plan is per-published-graph; compile failure path is safe; document the bound |
| R8 | Version bump `-g4` scope disputed | LOW | **Resolved** (§10.3 Q1: in scope, suffix-only); droppable without touching the engine if process disagrees |

---

## 10. Residuals & Open Reviews

### 10.1 G3 residuals — disposition this gate

| # | G3 residual | Disposition in G4 |
|---|---|---|
| G3-2 | Command payloads > 168 B have no queue path | **STANDS** — unchanged; side-channel remains future work |
| G3-5 | No real audio device callback — runner is a queue-consumer lane | **PARTIALLY CLOSED** — G4 ships a host virtual device (`sf_audio_engine_io_t` + tick/pacer) wired to the runner and the render harness; a *real* AAudio/OpenSL device remains deferred (G4-1) |
| G3-8 | TSan blocked under proot | **STANDS** (env) — re-attempt recorded on the snapshot/pacer tests; expected BLOCKED |
| G3-3 | Bit-exact cross-platform DSP equality not claimed | **STANDS** — true-peak tests use tolerances |
| G3-4 | Preset authoring deferred | **STANDS** — `dspPresetRef` still adds no render behavior |
| G3-7 | UTF-8 fail-open counting | **STANDS** — untouched |

### 10.2 New G4 residuals (documented, non-blocking)

| # | Residual | Sev | Disposition |
|---|---|---|---|
| G4-1 | Real Android audio device (AAudio/OpenSL) still not implemented; the device is a host virtual protocol | LOW (by design) | Future gate; `app/platform/audio/*` unchanged |
| G4-2 | True peak is a 4× polyphase approximation, not a certified ITU-R BS.1770 meter | LOW | Documented tolerances; revisit if certification is required |
| G4-3 | Pacer timing is best-effort (no RT priority/affinity under proot) | LOW (env) | Deterministic tick is the authoritative path; device/CI lane when available |
| G4-4 | Output target is fixed pre-start; no runtime retargeting | LOW | `configure`/`set_output` are pre-start by contract; revisit with a live-control command |
| G4-5 | Plan memory scales with node count (~4 KB/node pool) | LOW | Bounded by graph size; compile-failure path renders silence. **SEC-G4-08:** derived bound is 4 plans × N × ~4 KB (~16 KB/node) held live; OOM is contained (`planValid:false` + last-valid retention is the contract, `render_plan.cpp:105-108`); a compile-time node cap or pre-size check before pool build is a possible hardening, not a gate blocker |
| G4-6 | `std::atomic<double>` used for meter fields (lock-free on aarch64/x86; not guaranteed everywhere) | LOW | Per-field atomics + generation guard; document the assumption |
| G4-7 | Independent two-reviewer gate (@oracle + @security-reviewer) could not run in-environment (`subagent_depth`=1); §10.4 is a single-reviewer pass | LOW (process) | **RESOLVED at gate time by the orchestrator:** the two-reviewer gate ran out-of-band (oracle `ses_f5e210e6…` = P1/P2 PASS + plan executable; security `ses_f5e1130f…` = APPROVE-WITH-AMENDMENT, 10 findings SEC-G4-01…10 below). Amendments SEC-G4-01/02/05 are mandatory before P4; the rest are documented residuals |
| G4-8 | `sf_project_destroy` (void, SEC-G3-2 pattern) cannot see an attached engine in CREATED/stopped state (SEC-G4-06) | LOW (posture) | Same posture as the G3 IDLE-runner hole; document in `sf_audio_engine.h` + §4.5 that engine destroy is mandatory before project destroy; optional hardening = refuse free while `audioEngine != nullptr`; P4 ordering test |
| G4-9 | `alloc_counter` replaces only default-aligned throwing `operator new`/`new[]` (SEC-G4-07) | LOW | Over-aligned allocations would evade the zero-alloc assertion (false negative); add aligned `new`/`delete` overloads or comment the default-alignment precondition; file stays test-only (referenced only from tests/unit, never a production target) |
| G4-10 | Callback error/rollback paths unspecified (SEC-G4-09) | LOW | Specify: any non-`SF_OK` from `io.read` → render silence (never abort); non-`SF_OK` from `io.write` → block skipped; a throwing C++ callback is contained (catch → treated as non-`SF_OK`); pacer-spawn failure after runner start → stop+join the runner, return `SF_E_IO`, restore CREATED (rollback detail pinned in §4.5 `start`, SEC-G4-05) |

### 10.3 Open questions — resolved (pre-execution review)

| # | Question | Resolution |
|---|---|---|
| Q1 | Is the `0.1.0-g4` version bump in scope? (§5.3) | **YES — keep.** Suffix-tag convention is already established (G3 §5.3); it is a string/suffix change only (no wire, no `SF_SCHEMA_VERSION` change). Low-risk and review-visible. |
| Q2 | Is the host `float* const*` IO protocol acceptable as a *host-only* surface (no JNI)? (D5) | **YES.** G2 §1.3 forbids `float[]` across **JNI**; `sf_audio_engine_io_t` is a host-only C struct with no JNI export and no `NativeBridge` mirror (grep stays 26). |
| Q3 | 3-slot wait-free publish vs single-slot seqlock? (D1) | **AMENDED to 4-slot** (review R-A). A 3-slot acquire-then-release protocol is not wait-free (1 WRITING + 2 transient READING = all 3 blocked). 4 slots restores wait-free publish with no protocol change. Seqlock rejected: a single-slot seqlock forces the reader to retry on every publish and the writer to wait for reader quiescence, trading writer starvation for reader starvation and a torn-read window. |
| Q4 | Should the end-to-end engine+runner test live in unit or integration? (§7.2) | **Unit** (`tests/unit/test_audio_engine.cpp`) — the whole engine API is host-linkable and the runner integration needs no filesystem; keeps one harness and one CMake target. Revisit only if isolation proves necessary. |

### 10.4 Adversarial review record

> **Note on review provenance.** The plan called for @oracle + @security-reviewer
> subagents; subagent delegation is **unavailable in this environment**
> (`subagent_depth` limit = 1). This section is therefore a **single-reviewer
> adversarial pass** performed directly against the sources at `e252dbe`, and is
> marked as such for honesty. It is *not* a substitute for the independent
> two-reviewer gate if that gate is required by the project's process; the
> findings below are nonetheless evidence-backed and each cites `file:line`.
>
> **Gate-time update (orchestrator, post-draft):** the independent two-reviewer
> gate DID run out-of-band at execution time (oracle + security-reviewer over
> the committed plan and P1/P2). Results: oracle — **plan executable as-is,
> P1 PASS, P2 PASS** (findings ORC-G4-01…06, no HIGH, recorded below);
> security — **APPROVE-WITH-AMENDMENT** (findings SEC-G4-01…10; SEC-G4-01/02/05
> mandatory before P4; SEC-G4-03 folded into P3; remainder documented in §10.2
> as G4-8…G4-10). P3 carries ORC-G4-03/SEC-G4-03 (exception containment +
> compile-failure tests) and P4 carries SEC-G4-01/02/04/05/06/10.

| # | Decision (ref) | Verdict | Findings / amendments |
|---|---|---|---|
| **R-A** | Snapshot handoff (D1) | **OK-WITH-AMENDMENT (was a liveness defect)** | The 3-slot acquire-then-release protocol was **not wait-free**: ACQUIRE CASed new→READING *before* releasing the old, so the reader could transiently hold 2 READING slots; with 1 WRITING that is 3/3 non-claimable and PUBLISH spins. **Fix: 4 slots** (≤1 WRITING + ≤2 transient READING = 3 < 4 → always a claimable slot). Also: compile failure must restore `EMPTY`, not publish a null plan over a READY slot. Reader never touches `proj->doc` — **confirmed**. |
| **R-B** | `RenderPlan` seam (D4) | **OK-WITH-AMENDMENT** | **Layering defect:** the plan put `render_plan.{hpp,cpp}` in `src/dsp/`, but `compile_render_plan` needs `SignalGraphDoc`/`topological_order`/`find_node` (`graph_internal.hpp`) and would invert `sfgraph → sfdsp` into a **cycle**. **Fix: move to `src/graph/`** (compiled into `sfgraph`); `sfdsp` stays a leaf. Semantics: G3's `std::map` iteration is only used for *lookups*, and pred accumulation order is fixed by the topo walk, so the planned path is operand-order-equivalent — P1 must pin this per case. "Zero alloc" is achievable but must be asserted by both source scan **and** an allocation counter. Null-plan policy clarified (retain last valid plan). |
| **R-C** | True-peak semantics (D3) | **OK-WITH-CLARIFICATION** | The fs/4 + π/4 → +3.01 dB claim is **correct** for a 4×-grid-aligned crest (samples land at `A/√2`, continuous crest `A`); P2 must generate the tone on the exact grid and treat off-grid residual within the ±0.1 dB tolerance. `clipped = latch ≥ 1.0` is the G4 contract (not −1 dBTP — G4-2). Added passband-ripple false-clip bound and **NaN/Inf latch-poison** handling (ordered compare). |
| **R-D** | Engine lifecycle + runner coupling (D5) | **OK-WITH-AMENDMENT (real ownership hazard)** | `sf_queue_runner_create` only *checks* `runnerState == IDLE` and does **not** reserve it (`command_queue_thread.cpp:199-203`), so two engines could both pass create; and `sf_queue_runner_destroy` also only checks `runnerState`, so it cannot distinguish owners. **Fix: internal `SfProject::audioEngine` atomic CAS claim** in engine create (rolled back on failure); **no new export/code**. Destroy ordering made explicit (pacer join → runner stop/join → observer detach → runner destroy → free); destroy rejects STOPPING; `tick` rejected while PACE active. Caller must not free `p` before engine destroy (same borrowed-handle posture as the runner). |
| **R-E** | No new JNI / host `float*` (D5) | **OK** | `grep -c` of JNI exports = **26** at `e252dbe` (verified). The 11 new exports are `extern "C"` host-only; `float* const*` appears only in `sf_audio_engine_io_t`, which no JNI function exposes. No G0–G3 signature change; no new `SF_E_*` (7 codes at `sf_types.h`, all reused). |

**Additional findings (independently raised; all folded into the plan):**

| ID | Sev | Finding | Fix (already applied) |
|---|---|---|---|
| B-1 | MED | `sfmeasurement` INTERFACE target is the shipped name; plan said `sfmeasure` without noting the rename, and the top-level `add_subdirectory` loop configures `src/measurement`. | P2 file list + §2 note the explicit rename `sfmeasurement → sfmeasure`. |
| B-2 | MED | `sfaudio` link list in §2 omitted `sfgraph`, but the engine calls `render_chain_planned`/`compile_render_plan` (in `sfgraph`). | §2 + P4 set `sfaudio → {sfcore, sfgraph, sfmeasure}`. |
| B-3 | MED | `true_peak.hpp` was implicitly tied to `AudioBlock` (`dsp_internal.hpp`), which would make `sfmeasure` depend on `sfdsp` and violate the stated leaf rule. | D3/P2 now specify raw `float* const*` channels; `sfmeasure` includes no SoundForge target and stays a leaf. |
| B-4 | LOW | `meter_json` buffer contract (cap=0, tiny cap, null term) was unspecified. | P4 acceptance + E-R-D note: `SF_E_NOMEM` on too-small cap, never over-write, null-terminate. **SEC-G4-01 (security gate, mandatory before P4):** split error codes (NULL `e`/`buf`/`cap==0` → `SF_E_INVALID_ARG` matching `sf_queue_runner_last_report`; too-small `cap>0` → `SF_E_NOMEM` + `buf[0]='\0'`, never a truncated payload); bounded local serialization with one sized copy; locale-independent formatting (`std::to_chars`-style, never `%g`/`%f` under comma-decimal locale). Full contract now in §4.3 |
| B-5 | LOW | `planValid` observability flag was only prose ("an observable meter/status flag"). | Added to the D3 JSON shape and P4 acceptance. |
| B-6 | LOW | G4 test-count estimate (~45, 232→~277) is an estimate; the true count is set by gtest-discovered cases. | §8 keeps "≈" and the evidence-first rule (growth is fine). |

**Finding discharge record (P6 finalization).** All gate-review findings are
discharged with their discharging phase. The "docs (P6)" rows are documentation
amendments only — the codepaths already landed and were tested in the phase
listed in the middle column.

| Finding | Sev | Topic | Discharged in | Disposition |
|---|---|---|---|---|
| ORC-G4-01 | LOW | `alloc_counter.{hpp,cpp}` absent from plan §2/P1 file lists | **P6 (docs)** | Added to §2 and the P1 file list (files: `tests/unit/alloc_counter.{hpp,cpp}`, NEW, test-only) |
| ORC-G4-02 | LOW | `test_dsp_render.cpp` declared EDIT but untouched | **P6 (docs)** | P1 note + §2 row: intentionally unchanged; equivalence proven via the facade (`dsp_render.cpp:40-45`) + planned value tests + structural operand-order identity |
| ORC-G4-03 (=SEC-G4-03) | MED | publish-path compile containment (incl. `topological_order`) + cycle/OOM tests | **P3** | Whole compile wrapped in `catch(...)` → slot restored `EMPTY` first, last-valid retained; cycle + injected-OOM tests; 4×-OOM exhaustion pin |
| ORC-G4-04 | LOW | self-loop `preds[i]==i` dead corner | **P6 (docs)** | Comment-level disposition in §4.4 (D4): `topological_order` rejects self-loops as cycles → unreachable; no code change |
| ORC-G4-05 | LOW | `ExecuteMatchesG3RenderChainBitForBit` compares facade-vs-planned (same path twice) | **P6 (docs)** | §7.1 note: retained as G3-contract regression; cross-path proof is the 8 untouched G3 `Render.*` cases + direct planned value tests |
| ORC-G4-06 | LOW | configured-empty output target = valid silence | **P4** | Engine treats `""`/NULL target as compiled-out (`out_index==-1`) + valid silence; tested |
| SEC-G4-01 | MED | `meter_json` buffer/error-code contract | **P4** | NULL `e`/`buf`/`cap==0` → `SF_E_INVALID_ARG`; too-small `cap>0` → `SF_E_NOMEM` + `buf[0]='\0'`; bounded `to_chars` serialize + null-term; locale test (`GTEST_SKIP` in proot) |
| SEC-G4-02 | MED | meter sync layer (single-thread `TruePeak`) | **P4** | Single engine mutex around process/latch/reset + `meter_json`/`reset_meters`; no kernel locks; PACE-vs-`meter_json` test under UBSan |
| SEC-G4-04 | LOW | defensive `block.n > kBlockMaxSamples` guard | **P4** | Entry guard in `render_chain_planned` + engine tick both-edge (`frames` 0/513) tests |
| SEC-G4-05 | LOW (listed blocking) | `start` ordered gate + snapshot-#0 rollback | **P4** | runnerState gate before compile, observer attach, full rollback (stop+join, `SF_E_IO`, restore CREATED), `ORC-G4P4-01` rollback-store null fix |
| SEC-G4-06 (=G4-8) | LOW | `sf_project_destroy` cannot see an attached engine | **P4 (documented)** | `sf_audio_engine.h` + §4.5: engine destroy is mandatory before project destroy; posture parity with the G3 IDLE-runner hole |
| SEC-G4-07 (=G4-9) | LOW | `alloc_counter` default-aligned only | **P1 (comment) / P6 (docs)** | Documented default-alignment precondition; test-only, no production target references |
| SEC-G4-08 (=G4-5) | LOW | plan memory bound | **P6 (docs)** | §10.2 G4-5: 4 plans × N × ~4 KB; `planValid:false` + last-valid retention is the OOM contract |
| SEC-G4-09 (=G4-10) | LOW | callback error/rollback paths | **P4** | `io.read` non-OK → silence; `io.write` non-OK → block skipped; throwing callback contained; pacer-spawn failure → stop+join + `SF_E_IO` + restore CREATED |
| SEC-G4-10 | LOW | `destroy(NULL)` / claim-release ordering | **P4** | `destroy(NULL)` = no-op; double-destroy documented UB; claim released before/independent of free |
| ORC-G4P3-01/02/03 | — | catch-handler allocation, real-store observer test, tautological seq assert | **P3** | EMPTY-restore-first + no-alloc handlers; real-store-as-observer test; multiset `{2,3,4,5}` check |
| ORC-G4P4-01/02 (+03/04/05) | — | rollback store null, plan entry bound, tick(513), locale, standalone-runner | **P4** | All discharged in the P4 fixer pass |
| ORC-G4P5-01 | LOW | settle `clipped` margin on 4.4e-8 coeff | **P5** | Non-blocking; deterministic, documented in `RELEASE_NOTES_G4.md` residuals |

**Overall verdict:** **APPROVE to start P1, with the R-A/R-B/R-D amendments
applied** (they are now inlined above). No unresolved HIGH finding remains:
R-A's liveness hole is closed by the 4-slot proof, R-B's layering cycle is
avoided by relocating the module, and R-D's ownership race is closed by the
internal claim. Per the gate rule, P1 may begin once this revised plan is
committed (P0). The independent two-reviewer gate should be run if the project's
process requires it; that is the one residual process item (G4-7).

---

*End of PLAN_G4.md*
