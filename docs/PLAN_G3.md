# SoundForge — Gate G3 Plan: Full Audio Gate — DSP Engine, Mixing Law, RT
# Queue-Drain Lane, DspChain Editor, Name Model, Depth Cap

> **Status:** DRAFT — planning deliverable only. Pending adversarial review by
> @oracle (architecture) + @security-reviewer (surface); the pre-review open
> list is §10.5. **No implementation happens in this document** — it is the
> contract for gate execution after review.
> **Supersedes:** `docs/PLAN_G0.md`/`PLAN_G1.md`/`PLAN_G2.md` for G3 scope only.
> G0/G1/G2 remain the contract for everything not changed here.
> **Deliverable changes:** the static routing-desk estimate becomes a *real,
> host-unit-testable DSP law* (power-sum/headroom model, real clamping,
> finite-block kernels); the G2 command queue gains a *live RT queue-drain lane*
> (`sf_queue_runner`) with ownership guards; the DSP-chain route becomes a real
> editor; the G1 rename byte-cap residual and `sf_scene_rename` land as the G3
> name model; the JSON *depth* cap becomes a real pre-parse mitigation; a
> *bounded* dirty-flag layer covers only the new UI fields.

---

## 1. G3 Scope & Non-Goals

### 1.1 Goal

G2 delivered the *definition, persistence, validation and structural
evaluation* of a signal graph (topological order, static route gains) and
explicitly parked the mixing law for G3 (G2-5). G3 delivers the audio gate:

1. **DSP engine + real mixing law** — `native/src/dsp/` becomes a real STATIC
   library (`sfdsp`) with finite sample-buffer kernels (`apply_gain`,
   `apply_pan`, `apply_gate`, `mix_bus`, `soft_limit`) and the *law* module
   (coherent peak sum, incoherent power sum, headroom, clip gate).
   `evaluate_mixer` in `routing.cpp` is augmented to report the law — additive
   JSON keys only, G2 single-source numbers preserved exactly.
2. **RT queue-drain lane** — the G2 `SfCommandQueue` (proven lock-free SPSC,
   G2 P5) gains a real consumer: `sf_queue_runner`, a ring-consumer thread that
   drains commands and applies them via `sf_graph_apply_batch` on its own
   thread, with ownership guards (one mutating thread — G0/G1 C3 upheld),
   destroy-guard, and the G3-reserved `SF_CMD_EVALUATE_MIXER` (7) implemented
   in the runner only.
3. **DspChainScreen activation** — the G0 placeholder becomes a real editor:
   per-node DSP-preset attach/replace/clear over the (now on-wire)
   `signalNode.dspPresetRef`, pure-view + ViewModel per the G2 P7 precedent,
   no `NativeBridge.*` in composables.
4. **Name-model residuals** — `sf_scene_rename` C export lands; scene name
   gets char-based caps and freeze tracking; the G1 byte-vs-char rename-cap
   residual is fixed additively (relaxation only).
5. **JSON depth-cap assessment** — the nlohmann depth gate, deferred since G1,
   is evaluated: an iterative string/escape-aware `scan_json_depth()` pre-parse
   gate ships as the mitigation; the third-party nlohmann fork is documented
   **BLOCKED** (no patch available in this environment).
6. **Dirty-flag UI layer (bounded)** — a reusable `DirtyField<T>` tracker lands,
   scoped strictly to the new G3 fields (DspChain preset rows + the new
   scene-name field). Existing G2 screens are untouched.

Deliverable order: **DSP law → queue lane → UI → name model → depth cap**, with
the parallelizable lanes flagged per phase (§6).

### 1.2 G3 MUST deliver

| Pillar | Scope | Deliverable |
|---|---|---|
| **DSP mixing law** | `native/src/dsp/` + `native/src/graph/routing.cpp` | Additive law keys on `evaluate_mixer` output (`powerGainLin`, `headroomDb`; `peakGainLin` re-defined as coherent worst case; `clipped` law-derived) — G2 keys/values stable superset |
| **DSP kernels** | `sfdsp` STATIC lib (`dsp_internal.hpp`, `law.cpp`, `kernels.cpp`) | Finite-block kernels, deterministic per-platform, host-unit-testable, **no** new public C ABI / JNI |
| **Chain-render harness** | `native/src/graph/dsp_render.cpp` (compiled into `sfgraph`) | Topo-order chain render over finite buffers — the host-testable proof the law + kernels compose |
| **RT queue-drain lane** | `sf_queue_runner.h` + `src/core/command_queue_thread.cpp` | create/start/stop/join/last_report; runner-thread `apply_batch`; `SF_CMD_EVALUATE_MIXER` (7) + `SF_CMD_STOP` (0) in runner only; ownership + destroy guards (SF_E_IO) |
| **DspChain editor** | `DspChainScreen.kt` + new ViewModel/mirrors | Real editor (attach/replace/clear existing `dspPresets` envelopes), pure view, seeds from `UiState.Ready` |
| **Name model** | `sf_scene_rename` + `utf8_char_count` helper + cap relaxations | Scene rename with char-based caps (≤200 code points / ≤800 B), `scene.update` audit reuse, freeze tracking; project/venue/node caps corrected bytes→code points (relaxation only) |
| **Wire: `dspPresetRef`** | schema `$defs/signalNode` + codec + validator + golden | Additive schema refresh (schemaVersion stays **2**), golden byte-identical, both SHA-256 pins updated same commit |
| **Depth gate** | `scan_json_depth()` pre-parse in `schema.cpp` | kMaxJsonDepth = 256 → `SF_E_SCHEMA` + `last_error` before nlohmann; no schema/wire change |
| **Dirty flags (bounded)** | `DirtyField<T>` (Kotlin) | Seed-from-Ready per-field dirty tracking on the new G3 fields only |

### 1.3 G3 MUST NOT deliver (defer, leave stubs/unchanged)

- **Preset authoring** — the editor attaches/replaces/clears refs to
  *existing* `dspPresets` envelope ids only; `dspPresets[].data` stays
  unformalized (envelope contract only, G2 posture).
- **Actual audio callback / device lane** — the runner is a *queue consumer
  thread*, not an audio device callback. `app/platform/audio/*` remains
  interface-only; no Android audio engine, no AAudio. The host-side *render
  harness* exists so the DSP law is unit-testable (G4 connects a real device
  callback to the runner/harness).
- **Sample buffers / `float[]` across the ABI** — no kernel exports, no new
  C ABI beyond `sf_scene_rename` (25 → **26** exports) and `sf_queue_runner_*`
  (host-side native only, **no JNI**).
- **Chain ORDER on the wire** — processor chain order is *derived* (nodes in
  topological order), never persisted.
- **Schema migration / new error codes** — schemaVersion stays **2**, no
  migration step, no `.bak`; **no new `SF_E_*` codes** (depth gate reuses
  `SF_E_SCHEMA`; runner guards reuse `SF_E_IO`).
- **Multi-scene / multi-venue documents** — scene stays a frozen-named
  singleton; G3 only adds the *rename mutator* `sf_scene_rename` and the name
  freeze invariant around it.
- **`*.sfasset` sidecars, powerGraph, array/acoustics/measurement/optimization
  stubs** — unchanged.

### 1.4 Gating rule

Same as G0/G1/G2: files listed as *stub* are ≤20 LOC and carry a
`// G0: stub — G1+ implements` comment; *new* files implement their bounded
contract only. No file >300 LOC (advisory; deviations recorded the way G1
recorded `project.cpp` 540). The DSP kernels are data-parallel leaf functions
and stay small by construction.

---

## 2. File / Module Breakdown

Canonical root: `/root/project/soundforge/`. Tags: **NEW** / **EDIT** /
**REGEN** (regenerated from the updated schema) / **KEEP** (unchanged).

```
soundforge/
├── native/
│   ├── CMakeLists.txt                    # EDIT — SF_BUILD_VERSION default "0.1.0-g1" → "-g3"
│   ├── include/soundforge/
│   │   ├── sf_version.h                  # EDIT — SF_ENGINE_VERSION_SUFFIX "-g1" → "-g3"
│   │   ├── sf_project.h                  # EDIT — + sf_scene_rename export (additive)
│   │   ├── sf_command_queue.h            # EDIT — contracts C5/C6 rewritten, +C7/C8;
│   │   │                                 #        + SF_CMD_STOP (0); cmd 7 doc: runner-only
│   │   └── sf_queue_runner.h             # NEW  — sf_queue_runner_* C exports (5), contract
│   ├── src/dsp/
│   │   ├── CMakeLists.txt                # EDIT — INTERFACE stub → STATIC library sfdsp
│   │   ├── dsp_internal.hpp              # NEW  — kBlockMaxSamples=512, block struct,
│   │   │                                 #        kernel + law signatures (sfcore::dsp)
│   │   ├── law.cpp                       # NEW  — coherent_sum, power_sum, headroom_db, clip
│   │   └── kernels.cpp                   # NEW  — apply_gain / apply_pan / apply_gate /
│   │                                     #        mix_bus / soft_limit
│   ├── src/graph/
│   │   ├── CMakeLists.txt                # EDIT — target_link_libraries(sfgraph PRIVATE sfdsp)
│   │   ├── routing.cpp                   # EDIT — evaluate_mixer: additive law keys via
│   │   │                                 #        sfcore::dsp::merge_law (single-source
│   │   │                                 #        numbers byte-compatible with G2)
│   │   ├── dsp_render.cpp                # NEW  — chain-render harness (compiled into sfgraph)
│   │   ├── graph.cpp                     # EDIT — node label cap: 64 bytes → 64 code points
│   │   │                                 #        via utf8_char_count (byte ceiling 256)
│   │   └── graph_abi.cpp                 # EDIT — runner-active guard on graph mutators
│   │                                     #        (SF_E_IO "project.busy: queue runner active")
│   ├── src/core/
│   │   ├── CMakeLists.txt                # EDIT — add command_queue_thread.cpp
│   │   ├── command_queue.cpp             # EDIT — apply_batch_impl split (SEC-G3-1): public
│   │   │                                 #        sf_graph_apply_batch = busy-reject when runner
│   │   │                                 #        active + forward to impl; sync path still
│   │   │                                 #        rejects 0/7; runner calls impl on its thread
│   │   ├── command_queue_thread.cpp      # NEW  — sf_queue_runner implementation (pthread)
│   │   ├── sf_internal.hpp               # EDIT — SfProject runner lifecycle state machine
│   │   │                                 #        {IDLE,RUNNING,STOPPING,STOPPED} + atomic fast-
│   │   │                                 #        reject; explicit copy ctor (clone ⇒ IDLE,
│   │   │                                 #        ORC-1); utf8_char_count(); kMaxJsonDepth=256
│   │   ├── schema.cpp                    # EDIT — scan_json_depth()/checked_parse() pre-parse
│   │   │                                 #        gate; node-item validator mirrors
│   │   │                                 #        dspPresetRef additivity + type check (G3-10)
│   │   ├── migration.cpp                 # EDIT — checked_parse() at migrate_json parse site
│   │   │                                 #        (SEC-G3-7): 8 MiB cap + depth gate on all
│   │   │                                 #        4 raw-JSON entry points
│   │   ├── json_codec.cpp                # EDIT — write/read signalNode.dspPresetRef ("" ↔ null)
│   │   └── project.cpp                   # EDIT — sf_scene_rename export; rename caps
│   │                                     #        bytes→code points (project/venue);
│   │                                     #        sf_project_destroy runner guard (SF_E_IO)
│   └── data/schemas/project_schema.json  # EDIT — §5.1: signalNode + dspPresetRef (additive)
├── app/
│   ├── src/main/cpp/jni_bridge.cpp       # EDIT — + sceneRename (25 → 26 exports), header comment
│   ├── platform/bridge/NativeBridge.kt   # EDIT — + sceneRename external fun mirror
│   └── ui/
│       ├── common/DirtyField.kt          # NEW  — reusable seed/commit dirty tracker
│       ├── project/ProjectViewModel.kt   # EDIT — sceneRename commit helper; readyState parses
│       │                                 #        dspPresetRef; exposes scene name + preset rows
│       ├── signal/SignalKt.kt            # EDIT — SignalNodeKt + dspPresetRef
│       ├── dsp/
│       │   ├── DspChainKt.kt             # NEW  — typed mirrors (preset row per node)
│       │   ├── DspChainViewModel.kt      # NEW  — g1.1-hardened (@Volatile handle, nativeMutex)
│       │   └── DspChainScreen.kt         # EDIT — G0 placeholder → real editor (pure view)
│       ├── scene/SceneEditorScreen.kt    # EDIT — split: real scene-name field (DirtyField)
│       │                                 #        + existing project-name rename preserved
│       └── navigation/NavGraph.kt        # EDIT — dsp route wires DspChainViewModel;
│                                         #        scene route passes sceneName + rename lambda
├── python/soundforge_py/
│   ├── __init__.py                       # EDIT — __version__ "0.1.0-g1" → "0.1.0-g3"
│   └── migrate.py                        # EDIT — ENGINE_VERSION "0.1.0-g1" → "0.1.0-g3" (no new step)
├── tests/
│   ├── fixtures/
│   │   └── project_dspchain_v2.json      # NEW  — 3 nodes incl. dspPresetRef (P5 wire me proof)
│   ├── golden/
│   │   ├── schema_golden_v2.json         # REGEN — P5: byte-identical copy of refreshed schema
│   │   └── schema_golden_v1.json         # KEEP  — SHA-256 pin unchanged
│   ├── unit/
│   │   ├── test_dsp_law.cpp              # NEW  — coherent/power/headroom/clip law
│   │   ├── test_dsp_kernels.cpp          # NEW  — kernel math, determinism, no NaN/Inf
│   │   ├── test_dsp_render.cpp           # NEW  — chain render over finite blocks
│   │   ├── test_queue_runner.cpp         # NEW  — lifecycle, drain, evaluate, guards
│   │   ├── test_name_model.cpp           # NEW  — sf_scene_rename + utf8_char_count + caps
│   │   ├── test_json_depth.cpp           # NEW  — scan_json_depth gate (256/257, strings)
│   │   ├── test_graph_mixer.cpp          # EDIT — multi-source law keys added
│   │   ├── test_graph_nodes.cpp          # EDIT — label cap code points (multibyte)
│   │   ├── test_scene_venue.cpp          # EDIT — G1 caps relaxed (rename tests updated)
│   │   ├── test_cmd_queue.cpp            # EDIT — STOP intercept, cmd 7 runner-only note
│   │   ├── test_schema_validate.cpp      # EDIT — dspPresetRef accepted; depth gate
│   │   └── test_version.cpp              # EDIT — engine version "0.1.0-g3"
│   ├── integration/
│   │   └── test_graph_roundtrip.cpp      # EDIT — dspPresetRef + scene name survive reopen
│   └── python_tests/test_schema_py.py    # EDIT — P5 digest pin refresh (same commit), fixture
└── docs/
    └── RELEASE_NOTES_G3.md               # NEW  — gate artifact (P8)
```

**KEEP (explicitly untouched):** `native/src/audio/*` (stub),
`native/src/{acoustics,arrays,power,render,measurement,optimization}/`
(stubs), `native/data/migrations/*`, `tests/golden/schema_golden_v1.json`,
`sf_graph.h` signatures (additive only), `sf_command_queue.h` C ABI
signatures, `app/platform/audio/*` interface-only files, `powerGraph` in
schema/codec.

> **Bound check:** if a path above is not listed, do not create it in G3.

---

## 3. Environment & Validation Policy

Same container as G0–G2: proot/Termux on aarch64, gcc 14.2.0, clang 21.1.8,
cmake 4.4.3, python 3.13.5, JDK 21, 7.5 GiB RAM. No Android SDK/NDK/adb.

**Validation-domain mapping** (evidence-first; statuses in capitals):

| Domain | Meaning this gate | Evidence |
|---|---|---|
| **dev** | Host editors/VMs only; Kotlin is statically reviewed, never executed | Static review notes, `grep` gates |
| **native** | X e platform that runs and proves: ctest host build (reg) + UBSan build (`native/build-asan`, `-fsanitize=undefined -fno-sanitize-recover=all`) | `ctest` pass counts, UBSan logs |
| **native → Python** | Schema/audit/wire mirrored via the canonical-file reader | `pytest tests/python_tests -q`, drift `diff` |
| **integration** | Save→reopen, batch/runner end-to-end on host | `test_graph_roundtrip.cpp` + runner tests |
| **real target** (Android) | **NOT VERIFIED by design** — no SDK/NDK/adb; phases touching Android runtime are marked `NOT VERIFIED` (env) | Export-grep parity + static review + No-NativeBridge gate only (G2-4 CI-device-lane residual) |
| **release** | Gate tag + release notes + residuals disposition | `g3-complete` tag, `RELEASE_NOTES_G3.md` |

Hard constraints carried from G0/G1/G2 (unchanged):

- **ASan is dead under proot** (shadow-region init CHECK in
  `sanitizer_allocator_primary64.h`); **UBSan is the sanctioned sanitizer**.
  Threaded races on the runner are additionally bounded by explicit atomic
  flags + join-before-destroy tests.
- **Additive ABI only.** No signature change, no key removal, no error-code
  renumbering — relaxations only (name caps bytes→code points).
- **Errors via `set_last_error` + `SF_E_*`**; no exceptions across the JNI
  boundary (`SF_CATCH_ERRORS`), `nothrow new` → `SF_E_NOMEM`.
- **One commit per phase**, each leaving ctest (reg + UBSan) + pytest green.
- **Schema canonical == golden byte-identical**; Python mirror reads the
  canonical file (no copy); schemaVersion stays **2**; any wire refresh
  regenerates the golden and both SHA-256 pins **in the same commit**.
- **Stop-and-report** on any required-validation failure (§6 acceptance not
  met in a phase → do not proceed to the next phase).

---

## 4. Architecture Decisions

Each decision: **Decision** (what lands) / **Rationale** (why) /
**Review pointers** (what the adversarial reviewers must verify — §10.5).

### 4.1 D1 — The mixing law (replaces G2-5 desk estimate)

**Decision.** The G2 "static routing-desk estimate" becomes the G3 **law**,
implemented once in `sfdsp` (`sfcore::dsp::merge_law`) and called from
`routing.cpp`. Per-node path gain is unchanged: `gainLin = 10^(gainDb/20)`.
Per (source, output) route: `gainLin` = Σ over distinct paths of the path
product — **already a coherent within-source merge; unchanged, numbers
preserved byte-for-byte**. Output-level keys are **additive**:

```jsonc
// evaluate_mixer per-output object (G2 keys present, values stable superset):
{
  "nodeId": "<outputId>",
  "routes": [ { "sourceId": "...", "gainLin": 1.414, "nodeIds": ["..."] } ],
  "peakGainLin": 1.414,     // re-defined: coherent worst case = Σ over ALL source
                            // routes (all sources in phase). Single-source graphs
                            // (all current fixtures/tests) → identical to G2 value.
  "powerGainLin": 1.225,    // NEW: incoherent/uncorrelated estimate = sqrt(Σ gainLin²)
  "headroomDb": -3.01,      // NEW: -20·log10(peakGainLin); null when routes is empty
  "clipped": false          // law-derived: peakGainLin > 1.0 ⇔ headroomDb < 0
}
```

`power(peak) ≤ powerGainLin ≤ peak` for all gains ≥ 0, so the two keys bracket
the true level between full correlation and uncorrelated energy. **Real
clamping** (scope item) lives in the kernels: `soft_limit` (tanh knee + hard
ceiling) applied in the render harness, gated by the law-derived `clipped`.

**Rationale.** G2-5 explicitly parked the law for G3 with the G2 JSON as
stable input. Re-defining only the *output* aggregation (max-over-sources →
coherent sum) is the minimal, physically-meaningful correction: parallel
paths within one source were already summed coherently in G2; across-source
merging takes the coherent worst case for the desk and adds the incoherent
estimate — no consumer of the G2 shape breaks, and single-source numbers
(the dominant case) are identical, so the G2 mixer tests stay green.

**Review pointers.** (a) the single-source invariance argument; (b) `headroomDb
= null` vs 0 semantics on empty routes; (c) `clipped ⇔ headroomDb < 0` is a
pure refactor of the G2 threshold `peak > 1.0` — no fixture changes expected;
(d) mixture with negative/pan gains — pan is a stereo law (D2), not folded
into the scalar `gainLin` path product.

**Verdict R-A (oracle, 2026-09-13): OK.** Single-source invariance verified
against actual tests — every `peakGainLin` assertion in
`tests/unit/test_graph_mixer.cpp` (85, 126, 195, 234) is single-source or
empty-route; for one source the new Σ equals the old max (identical double).
`headroomDb` null-on-empty is *required*, not a choice: empty routes → peak 0 →
`-20·log10(0) = +∞`, and nlohmann dump-of-inf is implementation-defined —
document as `number|null`; Kotlin parse must tolerate null. `clipped ⇔
headroomDb<0` ≡ G2's `peak > 1.0` (log10 strictly increasing) — pure refactor.
Pan omission is safe-directional: equal-power pan bounds each channel by gain
(cos²θ, sin²θ ≤ 1), so scalar `peakGainLin`/`powerGainLin` never *understate*
per-channel clip risk. **Amendment:** insert the new keys alphabetically in the
per-output dump (routing.cpp:249-254 currently `clipped, nodeId, peakGainLin,
routes` — keep "sorted keys per level" honest); state the per-channel-bound
property in the law spec; revisit only when the render path reports per-channel
true peaks (G4).

### 4.2 D2 — `sfdsp` module API & the chain-render harness

**Decision.** `native/src/dsp/CMakeLists.txt`: `INTERFACE` → `STATIC` library,
pure C++20, `namespace sfcore::dsp`, **no public C ABI exports and no JNI**
(the ABI surface grows only by `sf_scene_rename`, 25 → 26). Files:
`dsp_internal.hpp` (`kBlockMaxSamples = 512`, `AudioBlock { float L[], float
R[]; int n; double gainLin; }`, kernel/law signatures), `law.cpp`
(`coherent_sum`, `power_sum`, `headroom_db`, `clip_gate`), `kernels.cpp`:

| Kernel | Contract |
|---|---|
| `apply_gain(L,R,n,g)` | scale by linear gain `g` |
| `apply_pan(L,R,n,pan)` | equal-power: θ = π/4·(pan+1), pan ∈ [−1,1]; L = cos θ, R = sin θ → **L²+R² = 1** |
| `apply_gate(L,R,n,on)` | mute/solo role: multiply by 0/1 |
| `mix_bus(dstL,dstR,srcL,srcR,n,g)` | accumulate `src·g` into `dst` |
| `soft_limit(L,R,n,knee,ceil)` | tanh knee, hard ceiling — finite outputs for any finite input |

Deterministic **per platform**; tests assert with relative tolerances;
**bit-exact cross-platform equality is NOT claimed** (residual G3-3). The
**chain-render harness** is `native/src/graph/dsp_render.cpp`, compiled *into*
`sfgraph` (which `PRIVATE`-links `sfdsp`; no cycle — `sfdsp` depends on
nothing). It walks the topological order and renders a caller-supplied finite
block through each node's kernels. G3 uses the harness for host unit tests
only; G4 connects a real device callback to it.

**Rationale.** Kernels owned by `src/dsp` (the G0 placeholder's purpose) and
law shared with `routing.cpp` avoids the G2-5 "desk estimate diverges from the
engine" hazard: both the desk and the render path call the *same* legal
`sfcore::dsp` functions. No kernel ABI/JNI keeps the C surface additive and
avoids `float[]` across the boundary (G2 §1.3 posture).

**Review pointers.** (a) pan law monotonicity + L²+R²=1 invariant across the
sweep; (b) `soft_limit` determinism and that hard ceiling never emits NaN/Inf
(∞/finite inputs); (c) `sfgraph` linking `sfdsp` cannot create an include or
link cycle; (d) no `float[]` in any public signature.

**Verdict note (oracle R-A(c), 2026-09-13):** equal-power pan bounds each
channel by the scalar gain (cos²θ, sin²θ ≤ 1) — stated in the law spec as the
**per-channel-bound property**: scalar `peakGainLin`/`powerGainLin` never
understates per-channel clip risk. Revisited at G4 when the render path reports
per-channel true peaks.

### 4.3 D3 — The RT queue-drain lane (`sf_queue_runner`)

**Decision.** New `native/include/soundforge/sf_queue_runner.h` (5 exports) +
`native/src/core/command_queue_thread.cpp`:

```c
/* Exclusive owner of the project handle's mutation thread while started (C7). */
sf_result_t sf_queue_runner_create(sf_queue_runner_t** out, sf_cmd_queue_t* q, sf_project_t* p);
sf_result_t sf_queue_runner_start(sf_queue_runner_t* r);        /* spawns pthread */
sf_result_t sf_queue_runner_stop(sf_queue_runner_t* r);         /* atomic flag; drains in-flight batch, then exits */
sf_result_t sf_queue_runner_join(sf_queue_runner_t* r);         /* reaps; idempotent; safe no-op if not running */
sf_result_t sf_queue_runner_last_report(const sf_queue_runner_t* r, char* buf, size_t cap);
```

Loop: non-blocking `sf_cmd_queue_dequeue`; empty → `std::this_thread::yield()`
+ ~1 ms sleep (idle poll; no busy spin); drain up to `kRunnerBatch = 64`
commands into a stack buffer, then `sf_graph_apply_batch` **on the runner
thread** (the sanctioned internal consumer — the "queue-drained clone path"
promised by G2 C6). Intercepts **before** `apply_batch`:

- **`SF_CMD_EVALUATE_MIXER` (7)** — runner calls internal `evaluate_mixer(g)`
  and stores the result JSON in `last_report` (mutex-guarded single slot,
  fixed 8 KiB copy, bounded). `sf_graph_apply_batch` **still rejects 7** on
  the sync path (G2 contract unchanged).
- **`SF_CMD_STOP` (0)** — new *control* command, no payload; the runner exits
  its loop without touching the project. Optional convenience over
  `sf_queue_runner_stop`; documented in `sf_command_queue.h`.

Ownership guards (all `SF_E_IO` + `last_error`):

| Condition | Error string |
|---|---|
| `sf_queue_runner_start` on a handle with an active runner | `"project.runner: already active"` |
| Any mutating `sf_graph_*` / `sf_project_*` ABI call while runner active | `"project.busy: queue runner active"` (fast path: atomic `runner_active` flag in `SfProject`; `apply_batch` internal path bypasses it — it runs *on* the runner) |
| `sf_project_destroy` while runner active | `"project.destroy: queue runner active"` (stop+join first — C8) |

**Contract changes in `sf_command_queue.h`:** C1–C4 unchanged; **C5/C6
rewritten** (the queue now has a live consumer — the runner is G2's promised
G3 lane); **+C7** runner owns the handle's mutation thread while started
(G0/G1 "one mutating thread" rule upheld — external mutators reject, they do
not race); **+C8** stop+join before any external access/destroy.

**Rationale.** G2 shipped the queue as a tested primitive with C6 promising
exactly this clone path. Making the runner the *sole* mutating thread while
started preserves C3 by construction instead of by discipline (the G0
"never malloc/lock in the callback" promise extends to "the runner thread's
hot path is the same lock-free queue"). `EVALUATE` as runner-only keeps
queries off the sync mutation path and gives the UI a bounded single-slot
report channel.

**Review pointers (oracle + security).** (a) destroy-guard vs crash-on-use race
under stop/join ordering; (b) single-slot `last_report` overwrite semantics
(only latest result — documented, never queued); (c) the external-mutator
reject is a *behavioral* guard — can any internal path (audit rotator, health
check, close) call a mutator while active? (d) `pthread` availability on host
only — Android static-only (G2-4).

**Amendments D3-amd (gate review, 2026-09-13 — SEC-G3-1..5 + ORC-1..3):**

- **`apply_batch` split (SEC-G3-1, HIGH→resolved):** extract the loop body of
  `sf_graph_apply_batch` (`command_queue.cpp:135-223`) into internal
  `apply_batch_impl`; the public `sf_graph_apply_batch` entry = busy-reject when
  runner active (`SF_E_IO "project.busy: queue runner active"`) + forward to
  impl. The runner calls the *impl* on its own thread — no flag bypass, no
  thread-id hack, no TOCTOU. Sync path keeps rejecting 0/7.
- **Lifecycle state machine (SEC-G3-3):** `SfProject` gains
  `{IDLE, RUNNING, STOPPING, STOPPED}` (atomic). Transitions: `start` (CAS
  IDLE→RUNNING, else `SF_E_IO "project.runner: already active"`), `stop`
  (RUNNING→STOPPING, atomic), and **`RUNNING→STOPPED` only by the runner
  thread's epilogue** (after its last batch) before thread exit — so
  `join`-return ⊨ flag clear. Destroy allowed only in IDLE or STOPPED(joined);
  rejects in RUNNING/STOPPING (`project.destroy: queue runner active` —
  set_handle_error, see SEC-G3-2).
- **Destroy guard is void-safe (SEC-G3-2):** `sf_project_destroy` keeps its
  `void` signature (additive ABI); active-runner destroy = `set_handle_error` +
  ERROR log + skip free; caller observes via `sf_last_error(handle)`; a second
  destroy after stop+join is then mandatory (documented in the header).
- **Read-path guards (SEC-G3-4 + oracle R-B(c)): reject, not contract.** Every
  `doc`-touching read while RUNNING rejects: `sf_graph_evaluate_mixer`,
  `sf_graph_validate_graph` / `topological_order`, `sf_project_to_json`,
  `sf_project_health_check`, `sf_project_clone`, getters — same one-atomic-check
  `SF_E_IO "project.busy: queue runner active"`. Exactly two exceptions:
  `sf_last_error(p)` (runner never writes `lastError` — amendment) and destroy
  (state rule). `sf_project_save_to_path` is a **mutator** (writes audit +
  `modifiedAt`, project.cpp:246-253) → in the mutator guard list explicitly,
  with `sf_scene_rename` (new). Runner-internal queries (health, validate,
  evaluate) use the impl path.
- **STOP barrier (ORC-2):** the drain loop stops filling at the first
  `SF_CMD_STOP`; commands `[0..k)` in the buffer apply; commands behind STOP
  stay queued (documented; flushed on next start — see restart rule below).
  Test `ADD→STOP→ADD` ordering explicitly.
- **Runner destroy / restart / truncation (SEC-G3-5 + ORC-3):** 6th export
  `sf_queue_runner_destroy(r)` (host-only, no JNI): requires IDLE/STOPPED.
  Restart forbidden from STOPPED — `start` accepts IDLE only (one-shot
  lifecycle); the runner handle must not outlive its queue or project handle
  (header contract). `last_report` truncation at the 8 KiB cap adds an explicit
  `"_truncated": true` sentinel member to the report JSON (observable, tested).
- **Clone copy ctor (ORC-1):** `SfProject` gains an explicit copy ctor copying
  `doc` + `lastError` and default-constructing runner state (clone ⇒ IDLE; clone
  is a read and rejects while RUNNING — read-guard). Test: clone IDLE → state
  IDLE; clone RUNNING → `SF_E_IO`.
- **Guard sweep completeness (ORC-4):** `core/CMakeLists.txt` adds
  `Threads::Threads` link for `command_queue_thread.cpp`; the P4 guard table
  lists all 14 mutators (6 `graph_abi.cpp` + 7 `project.cpp` incl.
  `save_to_path` + `sf_scene_rename`); `command_queue.cpp` moves KEEP→EDIT.

### 4.4 D4 — Slot size: G2-2 closed, keep 176 B / capacity 256

**Decision.** `sf_cmd_t` stays 176 B (content 168), capacity 256 = 44 KiB.
The G3 command set adds `SF_CMD_STOP = 0` (type only, no payload) and the
already-reserved `SF_CMD_EVALUATE_MIXER = 7` (no payload) — both trivially fit.
Command payloads larger than the slot (audio-config blobs) stay **deferred to
a side-channel design** (residual G3-2). The `SlotSizeIs176` test stays; the
"revisit with the G3 command set" comment in `sf_command_queue.h` is retired
with this decision noted there.

**Rationale.** No G3 command needs a bigger image; resizing the ring is a
review-visible constant change with no functional pressure, and the slot
payload ceiling is a *documented* property (G2-2 disposition).

**Review pointers.** Confirm no planned G3-era command requires >168 B of
content (attach/clear preset: id1 ≤ 64 B — fits).

### 4.5 D5 — Name model: `sf_scene_rename` + char caps + freeze tracking

**Decision.**

```c
/* native/include/soundforge/sf_project.h (new export, additive) */
sf_result_t sf_scene_rename(sf_project_t* p, const char* new_name);
```

- Validates: non-empty; **≤ 200 Unicode code points** and **≤ 800 bytes**
  (UTF-8); writes `doc.scene.name`; audit action **`scene.update`** (reuse —
  **no schema enum change**); `objectId` = scene.id; `modifiedAt` bump; log
  `INFO project "scene.update: rename"`.
- New shared helper in `sf_internal.hpp`: `size_t utf8_char_count(const char*)`
  — RFC 3629 code-point counting, **invalid UTF-8 counted fail-open** (each
  malformed byte counts as 1 char; documented + unit-tested).
- **Relaxations (additive-compatible, behavior-only):** rename caps move from
  *byte* counts to *code-point* counts — `sf_project_rename` / `sf_venue_rename`
  (200 bytes → 200 code points, byte ceiling 800) and `sf_graph_add_node`
  node label (64 bytes → 64 code points, byte ceiling 256). Strictly more
  names accepted; no signature changes; **G1 tests updated**
  (`test_scene_venue.cpp`, `test_graph_nodes.cpp`) and documented in
  `RELEASE_NOTES_G3.md`.
- **Scene-name freeze tracking:** `scene.name` is mutable *only* through
  `sf_scene_rename` (grep-audited invariant + a test asserting no other
  mutator writes `doc.scene.name`). Schema stays lenient (no `maxLength` on
  `scene.name` — legacy docs stay openable; precedent: `gainDb` range is
  native-only, G2).

**Rationale.** G1's "rename cap is bytes, not chars" residual lands here (G2
§10.1 re-deferred it to "G3 name-model work"). Scoping the *new* mutator to
code points while *relaxing* the two G1-era caps keeps the fixes additive; the
existing single-name model (scene name doubles as project name in the UI
today) is *split* in P6 without changing the native contract. Audit reuse of
`scene.update` avoids widening the schema enum (G2-1 watch-item discipline).

**Review pointers (security).** (a) fail-open byte counting — a hostile name
of all-invalid UTF-8 may pass where strict validation would reject; is the
posture right? (b) `utf8_char_count` must not overrun on truncated/broken
input; (c) `sf_scene_rename` must not touch `project.name` or any other field
(freeze invariant); (d) 800-byte ceiling keeps wire + audit detail bounded.

### 4.6 D6 — DSP chain on the wire (additive schema refresh)

**Decision.** `signalNode.dspPresetRef` becomes **on-wire** — nullable string,
`""`/null = none, must reference an existing `dspPresets[].id` (enforced at
`sf_graph_set_preset`, already implemented in G2 and in-memory today; only the
wire *drops* it — verified: `json_codec.cpp` writes/reads `kind/label/
position/mixer{mute,solo,gainDb,pan}` with no `dspPresetRef`).

- Schema: `$defs/signalNode` gains
  `"dspPresetRef": { "type": ["string", "null"] }` — additive property;
  `required` and `additionalProperties:false` otherwise unchanged (the new key
  is *in* the closed map, so the refresh is required — deliberate, reviewed in
  P5).
- Codec: write `n["dspPresetRef"] = node.dspPresetRef.empty() ? null string :
  value`; read tolerantly (`value("dspPresetRef","")`) — `0.1.0-g1/-g2` docs
  without the key open cleanly (legacy posture).
- Validator (schema.cpp) + Python mirror (reads canonical file) follow
  automatically; **golden regenerated and both SHA-256 pins updated in the
  same P5 commit** (G2-1 discipline).
- **Chain ORDER stays off-wire** — derived as processor nodes in topological
  order. `dspPresets[].data` body stays unformalized; no preset authoring
  (§1.3). Kotlin `SignalNodeKt` mirrors the new field; editor wires
  attach/replace/clear through `sf_graph_set_preset`.

**Rationale.** Without the wire, a saved/reopened project loses every preset
attachment (the same class of bug G2 P7's wire fix caught for mixer
values/edge ids — and *this* field was explicitly left off then). On-wire is
the review-clean choice: the field already exists in memory and mutation API;
only persistence is missing. Keeping ORDER derived and `data` unformalized
holds the G3 surface to exactly one additive property.

**Review pointers.** (a) golden regen + pin discipline (single commit, drift
green at every tree state); (b) `additionalProperties:false` — the refresh is
the *only* way to add the key, which is what makes it a reviewed event;
(c) codec null↔"" symmetry on write/read round-trip.

**Amendments D6-amd (gate review, 2026-09-13 — SEC-G3-9/10 + oracle R-C):** R-C
verified OK against actual code: drift currently clean, digest pin matches, the
codec drops `dspPresetRef` today (json_codec.cpp:117-128 write, 223-239 read).
- **Dangling-ref existence (SEC-G3-9):** add `dspPresetRef ∈ dspPresets[].id`
  to `sf_graph_validate` (graph_abi.cpp:171, beside the dangling-edge block
  194-204) + `sf_project_health_check` graph section (project.cpp:417-447) as an
  error; load stays tolerant (legacy posture, dangling-edge precedent); fixture
  with a dangling ref asserts the validate error; `""`-means-none must NOT trip
  the check (equals `sf_graph_set_preset(p,n,"")` clear semantics,
  graph_abi.cpp:129) — wire-level test required.
- **Native validator type-check (SEC-G3-10):** the `schema.cpp` signalNode
  check explicitly type-checks `dspPresetRef` as `string|null` (native/Python
  accept-reject parity); adversarial fixture (number-typed ref) asserted
  rejected by both hemispheres.

### 4.7 D7 — Dirty flags: in, bounded

**Decision.** New Kotlin `DirtyField<T>` (seed/commit/`isDirty`/`pristine`)
applied **only** to the new G3 fields: per-node preset rows in
`DspChainScreen` and the new scene-name field in `SceneEditorScreen`. Both
seed from `UiState.Ready` (committed-doc-is-truth, G2 P6 pattern). Commit
buttons enable only when dirty; on success the field re-seeds from the
refreshed document. Existing G2 screens (mixer sliders, geometry) are
untouched. Kotlin evidence = **static review** (no host Kotlin runtime).

**Rationale.** G1's "reseed-on-refresh discards unapplied field typing"
residual (G2 §10.1) is *mitigated for the new surfaces* without reopening the
g1.1 race-freedom contract: dirty state is pure view state; the document
remains authoritative, and a background refresh re-seeds *only*
non-interacted fields (dirty fields keep local typing, marked dirty).

**Review pointers.** (a) re-seed must never clobber an in-progress dirty edit
(compare `pristine` identity, not field equality alone); (b) dirty tracking
must not leak into ViewModel native commits (mutex discipline unchanged).

---

## 5. Schema & Wire Impact

### 5.1 Wire delta (v2.2 — additive, schemaVersion stays 2)

```jsonc
// native/data/schemas/project_schema.json — $defs/signalNode (additive property)
"signalNode": {
  "type": "object",
  "additionalProperties": false,
  "required": ["kind", "id", "label", "position", "mixer"],   // UNCHANGED
  "properties": {
    "kind":   { "type": "string", "enum": ["source", "processor", "output", "bus"] },
    "id":     { "type": "string", "format": "uuid" },
    "label":  { "type": "string" },
    "position": { "$ref": "#/$defs/point2" },
    "mixer":  { /* UNCHANGED: mute, solo required; gainDb, pan optional numbers */ },
    "dspPresetRef": { "type": ["string", "null"] }            // NEW (additive)
  }
}
```

- `signalEdge`: **unchanged** (`from`/`to`/`label` required, optional `id`;
  edge ports stay off-wire).
- Top level, `objectEnvelope` collections, `auditLog.action` enum (13),
  `signalMixerState`: **unchanged** — `sf_scene_rename` reuses `scene.update`.
- `dspPresets[].data`: unformalized (envelope contract only).

### 5.2 Golden & fixture discipline

- `tests/golden/schema_golden_v2.json` regenerated **byte-identical** to the
  refreshed schema; digest pin in `test_schema_py.py`
  (`test_golden_v2_immutable_digest`, currently
  `5888afd5…27aea`) updated **in the same P5 commit** — never opposite sides
  of a tree state (G2-1 watch-item).
- New fixture `project_dspchain_v2.json` (3 nodes: source → processor→ output;
  processor carries `dspPresetRef` pointing at an existing `dspPresets`
  envelope id; at least one node with `null`).
- `schema_golden_v1.json`: KEEP, pin untouched. Legacy docs (no key) open
  cleanly (tolerant codec read).

### 5.3 Version stamp — and the missed G2 bump

The G2 planned bump to `-g2` **never landed** (verified): `sf_version.h` still
`SF_ENGINE_VERSION_SUFFIX "-g1"`; `native/CMakeLists.txt` default
`set(SF_BUILD_VERSION "0.1.0-g1" CACHE STRING …)`; **both** build caches
(`native/build`, `native/build-asan`) have `SF_BUILD_VERSION:STRING=0.1.0-g1`
cached; `python/soundforge_py/__init__.py` `__version__` and `migrate.py`
`ENGINE_VERSION` are `0.1.0-g1`.

P1 therefore:
- Bumps all of the above to **`0.1.0-g3`** (suffix-tag convention, G2 §5.1
  choice stands — no minor bump).
- Documents the missed G2 bump in a release-note paragraph.
- Reconfigures both caches so the stamp takes effect:

```bash
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g3 native/build
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g3 native/build-asan
```

`SF_SCHEMA_VERSION` stays **2**; `sf_is_compatible` semantics unchanged; no
migration step (G2 §5.3 posture stands: untouched files keep the writer's
version, g1/g2 docs open without `.bak`/`project.migrate`).

### 5.4 Diagnostics & logging deltas (all via the existing `sf_log` ring)

| Condition | Level | Tag | Payload |
|---|---|---|---|
| Runner started / stopped | `INFO` | `runner` | `runner start/stop handle=<h>` |
| Runner last_report overwritten (evaluate) | `INFO` | `runner` | `evaluate ok seq=<n>` |
| Runner apply_batch partial failure | `ERROR` | `runner` | `apply_batch stopped at <k>/<n>: <err>` |
| External mutator rejected while runner active | `WARN` | `graph`/`project` | `busy: queue runner active` |
| `sf_project_destroy` with active runner | `ERROR` | `project` | `destroy: queue runner active` |
| `sf_scene_rename` success/failure | `INFO`/`ERROR` | `project` | `scene.update: rename` / `last_error` |
| Depth gate trip (pre-parse) | `ERROR` | `schema` | `json depth exceeds 256 (pre-parse)` |
| `sf_graph_add_node` label cap (code points, multibyte) | `WARN` | `graph` | `name too long (>64 chars)` |

---

## 6. Phases (P0…P8)

Strict main-line order; each phase leaves the tree green (ctest reg + UBSan +
pytest) and is **one commit**. Lanes: **A** = DSP/law/render/runner,
**B** = name model/wire/UI, **C** = depth. Parallelizable lanes are flagged;
execution is linear. Expected net-new native tests ≈ **55** (119 → ~174;
G2 taught us the actual will grow — that is fine, evidence-first). pytest
9 → ~11. JNI 25 → 26.

### P0 — Plan commit (Lane All, ½ day)
- [ ] **Goal:** reviewable contract — this file.
- [ ] **Files:** `docs/PLAN_G3.md` (this document).
- [ ] **Acceptance:** §1–§10 present and internally consistent; **adversarial
      review COMPLETE (2026-09-13)** — verdicts recorded in §10.5.1/.2,
      amendments folded into the phases/decisions, gate verdict §10.5.3 =
      APPROVED (no open HIGH findings); the review history is the P1 entry
      evidence.
- **Validation:** human review (no code). **Commit:** `plan(g3): PLAN_G3.md draft — DSP law + queue runner + DspChain UI + name model + depth cap`

### P1 — Name model + version stamp (Lane B, 1 day)
- [ ] **Goal:** `sf_scene_rename` + char caps + freeze tracking; `0.1.0-g3`
      everywhere including caches; the missed G2 bump documented.
- [ ] **Files:** `sf_project.h` (+`sf_scene_rename`), `project.cpp`,
      `sf_internal.hpp` (`utf8_char_count`, `kMaxJsonDepth` placeholder),
      `graph.cpp` (label caps), `sf_version.h`, `native/CMakeLists.txt`,
      python `__init__.py`/`migrate.py`, `test_name_model.cpp` (NEW),
      `test_scene_venue.cpp` + `test_graph_nodes.cpp` + `test_version.cpp`
      (EDIT).
- [ ] **Acceptance:** `sf_scene_rename` audit = `scene.update`, `modifiedAt`
      bumped, name survives save/reopen; 200/201 code-point boundary + 800/801
      byte boundary; multibyte label ≤64 cps accepted (was lenient in bytes —
      relaxation test both old/new); `utf8_char_count` invalid-UTF-8 fail-open
      cases; engine version string `0.1.0-g3` from a **reconfigured** build;
      no other mutator touches `scene.name` (freeze test).
- **Validation:** host ctest (reg + UBSan) + pytest. **Commit:**
  `p1(g3): scene rename + char caps + version stamp 0.1.0-g3 (missed g2 bump documented)`

### P2 — `sfdsp` static library: law + kernels (Lane A1, 1–1.5 days)
- [ ] **Goal:** the DSP module proper — kernels and law, host-unit-testable,
      nothing wired yet.
- [ ] **Files:** `dsp/CMakeLists.txt` (INTERFACE→STATIC), `dsp_internal.hpp`,
      `law.cpp`, `kernels.cpp` (NEW); `test_dsp_law.cpp`, `test_dsp_kernels.cpp`
      (NEW). `routing.cpp`/`dsp_render.cpp` NOT touched yet.
- [ ] **Acceptance:** pan invariant L²+R²=1 across −1..1 sweep +
      monotonicity; soft_limit finite for ±1e30 inputs, hard ceiling exact,
      no NaN/Inf; law bracket property power ≤ coherent; headroom −6.02 dB at
      peak 2.0 / null on no routes; kernels deterministic (two identical runs
      equal within 1e-12 on the same platform).
- **Validation:** host ctest (reg + UBSan). **Commit:**
  `p2(g3): sfdsp static lib — mixing law + finite-block kernels`

### P3 — Law integration + chain-render harness (Lane A2, 1 day)
- [ ] **Goal:** `evaluate_mixer` reports the law (additive keys, single-source
      numbers byte-compatible); the render harness proves kernels + law
      compose.
- [ ] **Files:** `routing.cpp` (via `sfcore::dsp::merge_law`),
      `graph/CMakeLists.txt` (link `sfdsp`), `dsp_render.cpp` (NEW, compiled
      into sfgraph); `test_dsp_render.cpp` (NEW), `test_graph_mixer.cpp`
      (EDIT — multi-source: `peakGainLin` = Σ, `powerGainLin` = √Σg²,
      `headroomDb`, `clipped` ⇔ `headroomDb<0`; existing single-source cases
      **untouched and green**).
- [ ] **Acceptance:** G2 mixer tests pass unmodified; new law assertions hold;
      render harness: 2-node chain (src gain −6 dB → out) renders a DC block to
      exactly half amplitude (1e-12); mute/solo exclusion; block-boundary
      determinism (1024 samples = 2×512 blocks, same output as one 512 block
      per segment — no inter-segment state bleed for stateless kernels).
- **Validation:** host ctest (reg + UBSan). **Commit:**
  `p3(g3): mixing law in evaluate_mixer + dsp_render chain harness`

### P4 — Queue runner lane + ownership guards (Lane A3 — parallelizable with P2/P3, 1.5–2 days)

Two commits on the same lane (review seam, oracle R-B verdict): **P4a = runner
module + lifecycle state machine + queue contract; P4b = the guard sweep**
(mutators, reads, save, destroy, clone) with its own acceptance. Each commit
leaves the tree green.

#### P4a — runner module + state machine + contract
- [ ] **Goal:** the live RT drain lane (G0 "no lock/malloc in the hot path"
      intact) + `{IDLE,RUNNING,STOPPING,STOPPED}` lifecycle; provable single
      owner.
- [ ] **Files:** `sf_queue_runner.h` (6 exports: create/start/stop/join/
      last_report/**destroy** — D3-amd), `command_queue_thread.cpp` (NEW),
      `sf_command_queue.h` (C5/C6 rewritten, +C7/C8, `SF_CMD_STOP`=0, STOP
      barrier semantics — ORC-2), `sf_internal.hpp` (state machine + explicit
      copy ctor — ORC-1), `core/CMakeLists.txt` (+`Threads::Threads` — ORC-4),
      `command_queue.cpp` (**apply_batch_impl split** — SEC-G3-1: public entry
      busy-rejects when runner active then forwards; runner calls impl),
      `test_queue_runner.cpp` (NEW), `test_cmd_queue.cpp` (EDIT).
- [ ] **Acceptance:** lifecycle create→start→stop→join→destroy; 10k `addNode`
      cmds enqueued while running → applied by the runner (10k nodes, 10k
      audits, seq dense); `EVALUATE` via cmd 7 → valid result JSON in
      `last_report` (bounded copy ≤8 KiB, `"_truncated": true` sentinel when
      truncated — ORC-3); `apply_batch_impl` split: sync `sf_graph_apply_batch`
      still rejects 0/7; `STOP` barrier: drain stops filling at first STOP,
      `[0..k)` apply, behind-STOP stay queued (`ADD→STOP→ADD` ordering test —
      ORC-2); double-start (CAS) → `SF_E_IO "project.runner: already active"`;
      start-from-STOPPED → rejected (one-shot — ORC-3); exhausted-drain join
      reaps within bounded time (no deadlock); UBSan clean.
- **Validation:** host ctest threaded (reg + UBSan) + one TSan attempt on the
      runner tests only (ASan-dead ≠ TSan-dead; if proot blocks it, record as
      residual G3-8). Android = static-only (`NOT VERIFIED` by design — G2-4).
      **Commit:** `p4a(g3): sf_queue_runner + lifecycle state machine + apply_batch_impl split`

#### P4b — the guard sweep (completeness-critical)
- [ ] **Goal:** every `sf_*` entry touching `doc`/`lastError` has a guard
      decision; the single-owner invariant is enforced at the ABI boundary.
- [ ] **Files:** `graph_abi.cpp` (mutator + read guards — R-B(c)),
      `project.cpp` (save/destroy/clone guards + `sf_scene_rename` guard),
      `sf_queue_runner.h` (contract: runner never writes `proj->lastError` —
      SEC-G3-4), `test_queue_runner.cpp` (EDIT — guard cases).
- [ ] **Acceptance — explicit per-site checklist:** all 14 mutating entries
      (6 `graph_abi.cpp` graph mutators + 7 `project.cpp` incl.
      `sf_project_save_to_path` + destroy + `sf_scene_rename`) reject
      `SF_E_IO "project.busy: queue runner active"` while RUNNING; all
      `doc`-reading entries (evaluate_mixer, validate, topo_order, to_json,
      health_check, clone, getters) reject likewise, **except** `sf_last_error`
      and destroy; `sf_project_destroy` while RUNNING/STOPPING =
      `set_handle_error("project.destroy: queue runner active")` + skip free
      (void signature — SEC-G3-2; second destroy after stop+join required);
      destroy in STOPPED(joined) proceeds; clone while RUNNING → `SF_E_IO`;
      clone in IDLE → runner state IDLE (ORC-1); JNI never starts a runner
      (documented in `RELEASE_NOTES_G3` — reads in RUNNING state are a
      contract violation).
- **Validation:** host ctest (reg + UBSan) incl. concurrent-mutator hammer
      during drain (all `SF_E_IO`, UBSan clean) + read-reject during RUNNING +
      stop→destroy-without-join→reject→join→destroy. **Commit:**
      `p4b(g3): guard sweep — mutator/read/destroy/clone single-owner enforcement`

### P5 — `dspPresetRef` on the wire (Lane B — prerequisite of P6, 1 day)
- [ ] **Goal:** preset attachments persist; schema v2.2 refresh under full
      golden discipline.
- [ ] **Files:** `project_schema.json` (signalNode +`dspPresetRef`),
      `json_codec.cpp` (write/read, "" ↔ null), `schema.cpp` (validator
      mirrors), `tests/golden/schema_golden_v2.json` (**REGEN**),
      `test_schema_py.py` (**both pins same commit**),
      `fixtures/project_dspchain_v2.json` (NEW), `test_graph_roundtrip.cpp`
      (EDIT — extends `GraphMixerAndEdgeIdsSurviveRoundTrip`),
      `test_schema_validate.cpp` (EDIT), `SignalKt.kt` (+`dspPresetRef`).
- [ ] **Acceptance:** drift `diff` empty after regen; digest pins updated **in
      this commit**; new fixture validates in native + Python; round-trip:
      setPreset → save → reopen → `dspPresetRef` equal, legacy doc without the
      key opens and reads `""`; `sf_graph_set_preset` validation unchanged
      (unknown id → `SF_E_NOT_FOUND`); **D6-amd fold-in:** ref-existence errors
      in `sf_graph_validate` (graph_abi.cpp:171) + `sf_project_health_check`
      graph section; dangling-ref fixture asserts the validate error; `""`-
      means-none passes (wire-level test — equals `sf_graph_set_preset(p,n,"")`
      clear semantics); native validator type-checks `dspPresetRef` string|null
      (SEC-G3-10); adversarial fixture (number-typed ref) rejected by **both**
      native and Python.
- **Validation:** host ctest + pytest + drift diff. **Commit:**
  `p5(g3): dspPresetRef on wire — schema v2.2 refresh + golden + pins`

### P6 — DspChain editor + scene-name field + dirty flags (Lane UI, 1–1.5 days, static-only)
- [ ] **Goal:** the G0 placeholder becomes a real editor; the scene screen
      gains a *real* scene-name field (project-rename behavior preserved);
      `DirtyField` covers the new fields only.
- [ ] **Files:** `DspChainKt.kt`, `DspChainViewModel.kt`, `common/DirtyField.kt`
      (NEW); `DspChainScreen.kt`, `SceneEditorScreen.kt`, `NavGraph.kt`,
      `ProjectViewModel.kt`, `jni_bridge.cpp` (+`sceneRename` → 26),
      `NativeBridge.kt` (+mirror) (EDIT).
- [ ] **Acceptance:** DspChainScreen lists nodes with preset chips
      (attach/replace/clear over existing `dspPresets` envelopes only), errors
      inline via `lastEditError`, **zero `NativeBridge.*` in composables**;
      scene screen has two distinct fields — "Scene name" (new, `DirtyField`,
      commits via `sceneRename`) and the existing "Scene/project name"
      (renames the project — unchanged behavior); dirty commit buttons enable
      only when dirty; re-seed never clobbers a dirty edit; export grep = **26**;
      ctest suite still green (native unaffected).
- **Validation:** export-grep parity + static Kotlin review (pure view,
      mutex discipline, close() joins — G2 §7.4 pattern). **Commit:**
      `p6(g3): dsp chain editor + scene-name field + dirty flags (jni 26)`

### P7 — JSON depth pre-parse gate (Lane C — parallelizable, 1 day)
- [ ] **Goal:** bound stack-exhaustion exposure on **all 4 raw-JSON entry
      points**; the nlohmann fork disposition recorded.
- [ ] **Files:** `sf_internal.hpp` (`kMaxJsonDepth = 256`, `checked_parse()`
      shared helper — SEC-G3-7), `schema.cpp` (`scan_json_depth()` — strict
      one-escape-char iterative, string/escape-aware, applied via
      `checked_parse()`), `project.cpp` (from_json + open_from_path move onto
      `checked_parse()`), `migration.cpp` (migrate_json onto `checked_parse()`
      — SEC-G3-7/ORC-4c), `test_json_depth.cpp` (NEW), `test_schema_validate.cpp`
      (EDIT).
- [ ] **Acceptance — scanner contract (SEC-G3-8):** pre-rejects ⊆ nlohmann
      rejects; accept-set equal; depth equal on every nlohmann-accept. Depth 256
      accepted / 257 rejected with `last_error` `"schema: json depth exceeds
      256"`; unterminated-string → distinct pre-reject text `"schema: unterminated
      string in json"` (never a false depth trip); unknown escapes (`\x`) handled
      as 2-char unit without skipping 2; odd/even backslash runs before a quote;
      `\uXXXX` with <4/non-hex digits never desyncs string tracking; raw control
      bytes in strings; braces/escapes inside strings do **not** affect depth;
      malformed JSON still yields the nlohmann result (scanner never re-implements
      the parser); differential fuzz over mutated 255/256/257-depth docs asserting
      scanner verdict ⊆ nlohmann outcome; all 4 parse sites gated (drives deep
      docs through `sf_validate_project_json` + `sf_migrate_json` too); 8 MiB byte
      cap + depth both pre-parse; UBSan green.
- **Validation:** host ctest (reg + UBSan). **Commit:**
      `p7(g3): checked_parse depth gate on all 4 json entries — nlohmann fork BLOCKED documented`

### P8 — Docs + DoD sweep + tag (Lane All, ½ day)
- [ ] **Goal:** gate artifact.
- [ ] **Files:** `docs/RELEASE_NOTES_G3.md` (NEW), this plan finalized,
      §10.5 verdicts resolved/re-deferred.
- [ ] **Acceptance:** full suite (§7.5) green: ctest reg + UBSan, pytest,
      drift empty, grep **26**; version stamp `0.1.0-g3` in both reconfigured
      caches; **§10.5 verdicts table: every SEC-G3-*/ORC-*/R-* row discharged
      (folded into the phases above) or re-deferred with a residual — the DoD
      checklist (§8) is the sweep source of truth**; tag `g3-complete`.
- **Validation:** evidence table in release notes (G2 style).
  **Commit:** `p8(g3): release notes + DoD sweep; tag g3-complete`

---

## 7. Test Plan

> Gate G3 exit: the mixing law is real and host-proven, the queue drain lane
> owns the mutation thread safely, preset attachments persist, the name model
> is char-correct, the depth gate bounds recursion, and the DSP-chain UI is a
> pure-view editor — with G0/G1/G2 suites still green.

### 7.1 Native unit tests (`tests/unit/`, via ctest)

| File | Notable cases |
|---|---|
| `test_dsp_law.cpp` (≈10) | coherent_sum single/multi source; power_sum bracket `max ≤ √Σg² ≤ Σg`; headroom math (±dB at 2.0/1.0/0.5, null on empty); clip gate ⇔ headroom<0; additive-shape guard (G2 keys still present on output). |
| `test_dsp_kernels.cpp` (≈10) | apply_gain scale; apply_pan: L²+R²=1 sweep + monotonicity + hard pan = 1/0; apply_gate 0/1; mix_bus accumulate over n blocks; soft_limit identity in linear zone, ceil exact, finite for ±1e30; no NaN/Inf for any finite input; double-run determinism ≤1e-12. |
| `test_dsp_render.cpp` (≈7) | 2-node chain DC block −6 dB → half amplitude; mute/solo exclusion through the chain; pan stereo sum = mono gain (equal-power); 1024-sample block segmentation determinism; empty graph → identity block. |
| `test_queue_runner.cpp` (≈18) | lifecycle create→start→stop→join→destroy; 10k drain; evaluate cmd 7 → last_report (+`"_truncated": true` sentinel); STOP barrier with `ADD→STOP→ADD` ordering; concurrent-mutator hammer during drain (all `SF_E_IO`); read-reject during RUNNING; double-start (CAS); start-from-STOPPED rejected; destroy-guard (void + set_handle_error) stop→destroy-without-join→reject→join→destroy; clone-during-RUNNING `SF_E_IO`; last_report ≤8 KiB; join-reaps-within-bounded-time (no deadlock); TSan attempt recorded; UBSan clean. |
| `test_name_model.cpp` (≈10) | scene rename happy path + audit `scene.update` + objectId = scene.id + modifiedAt bump; rename distinguishable in audit `detail` (SEC-G3-3 reuse verified); empty/whitespace reject; 200/201 cp and 800/801 B boundaries (ASCII + multibyte); invalid-UTF-8 fail-open counting (conservative: chars ≥ bytes); freeze invariant (no other mutator writes scene.name); utf8_char_count unit table (overlong, lone continuation, truncated, 4-byte). |
| `test_json_depth.cpp` (≈13) | 256 ok / 257 reject SF_E_SCHEMA + dedicated `last_error`; **distinct** unterminated-string pre-reject text; braces/escapes inside strings; escaped quotes/backslashes; odd/even backslash runs before a quote; unknown-escape 2-char unit (`\x`); `\uXXXX` with <4/non-hex digits (no string-state desync); raw control bytes in strings; nested mixed `{}`/`[]`; explicit-size `from_json`; **all 4 parse sites** gated (`from_json`, `open_from_path`, `sf_validate_project_json`, `sf_migrate_json`); differential fuzz scanner⊆nlohmann over 255/256/257-depth docs; byte-cap + depth both pre-parse; UBSan green. |
| `test_graph_mixer.cpp` (EDIT) | existing cases untouched; +multi-source: peakGainLin=Σ, powerGainLin=√Σg², headroomDb, clipped⇔headroomDb<0. |
| `test_graph_nodes.cpp` (EDIT) | label ≤64 **code points** (multibyte label that is >64 bytes but ≤64 cps now accepted; >64 cps rejected); cap error string updated (>64 chars). |
| `test_scene_venue.cpp` (EDIT) | G1 rename tests updated: 200-code-point boundary (was byte); multibyte names now accepted. |
| `test_cmd_queue.cpp` (EDIT) | `SlotSizeIs176` stays; +STOP intercept never reaches apply_batch; apply_batch still rejects 7; SeqWatermark/overflow tests unchanged. |
| `test_schema_validate.cpp` (EDIT) | `project_dspchain_v2.json` → SF_OK; depth-gate fixture cases. |
| `test_version.cpp` (EDIT) | engine version `0.1.0-g3`; `sf_is_compatible` table unchanged. |

### 7.2 Integration tests (`tests/integration/`)

| Test | Steps | Pass |
|---|---|---|
| `test_graph_roundtrip.cpp` (EDIT) | build graph + `sf_graph_set_preset` + `sf_scene_rename` → save → reopen → nodes/mixer/`dspPresetRef`/scene name equal; no `project.migrate`; schemaVersion == 2 | dspPresetRef + scene name survive reopen; no-migration proven |

### 7.3 Python tests (`tests/python_tests/test_schema_py.py`, EDIT)

```python
def test_dspchain_fixture_validates():   # project_dspchain_v2.json -> == []
def test_golden_v2_immutable_digest():   # NEW SHA-256 pin (P5 refresh, same commit as regen)
def test_golden_v1_immutable():          # existing pin — UNCHANGED
```

### 7.4 Android (static-only — no SDK/NDK, no `assembleDebug`, no logcat)

- `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` =
  **26** (25 + sceneRename) in `jni_bridge.cpp`; one mirror per external fun.
- Kotlin reviewed statically: `DspChainScreen`/`SceneEditorScreen` composables
  contain **no `NativeBridge.*`** (pure views — g1.1/G2 P7 rule); ViewModels
  own all native work under `nativeMutex`; `close()` joins before
  `projectDestroy`; `DirtyField` seeds from `UiState.Ready` only.
- Runner/pthread behavior proven on host only — Android runner lane is
  `NOT VERIFIED` by design (G2-4).

### 7.5 Verification commands (host)

```bash
cmake --build native/build -j2 && ctest --test-dir native/build --output-on-failure
cmake --build native/build-asan -j2 && ctest --test-dir native/build-asan --output-on-failure
python3 -m pytest tests/python_tests -q
diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json   # nothing
grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' app/src/main/cpp/jni_bridge.cpp  # 26
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g3 native/build          # P1 reconfigure (both dirs)
ctest --test-dir native/build -R Version                                  # sf_engine_version() == "0.1.0-g3"
```

---

## 8. Definition of Done (Gate G3)

All true on `main`:

1. **Builds:** ctest green on `native/build` (reg) **and** `native/build-asan`
   (UBSan substitute — ASan remains proot-blocked); pytest green; JNI export
   grep = **26**. Plan expects ≈55 net-new native tests (119 → ~174); growth
   beyond plan is fine when evidence-first, recorded as G2 recorded its own.
2. **No drift:** `project_schema.json` byte-identical to
   `schema_golden_v2.json`; both SHA-256 pins updated **in the same P5 commit**
   as the regen; `schema_golden_v1.json` untouched; schemaVersion still **2**.
3. **Version stamp `0.1.0-g3`:** `sf_version.h`, `native/CMakeLists.txt`
   default, **and both reconfigured caches**; python `__version__` +
   `ENGINE_VERSION`; the missed G2 bump documented in release notes.
4. **DSP law:** `evaluate_mixer` output is a stable superset of the G2 shape —
   every G2 key/value identical on single-source graphs (existing tests pass
   unmodified); `powerGainLin`, `headroomDb` added; `peakGainLin` documented
   as coherent worst case; `clipped` law-derived.
5. **Kernels:** all five kernels unit-tested (pan L²+R²=1, soft_limit ceiling,
   no NaN/Inf, determinism ≤1e-12); **no new public C ABI / JNI** (count stays
   26); `audio/*` stub untouched.
6. **Render harness:** finite-block chain render proves law+kernels compose;
   host tests only; no device lane.
7. **Queue runner:** C1–C4 unchanged, C5/C6 rewritten, C7/C8 added; **P4a/P4b
   seam committed**: lifecycle state machine `{IDLE,RUNNING,STOPPING,STOPPED}`
   (RUNNING→STOPPED only by runner epilogue), `apply_batch_impl` split (public
   entry busy-rejects then forwards; runner calls impl), destroy guard
   (void-safe: set_handle_error + skip free), read-path guards (reject, except
   `sf_last_error`), runner never writes `proj->lastError`, STOP barrier
   semantics, clone copy-ctor (clone ⇒ IDLE), runner destroy export +
   `"_truncated"` sentinel; lifecycle + 10k drain + evaluate-last_report +
   guard sweep (14 mutators + reads) all tested (host, incl. concurrent-mutator
   hammer); `apply_batch` sync semantics unchanged (still rejects 0/7 on the
   sync path); TSan attempt recorded (blocked → residual G3-8).
8. **Name model:** `sf_scene_rename` shipped (audit `scene.update`, no schema
   enum change; rename distinguishable in audit `detail`); char caps via
   `utf8_char_count` (fail-open documented, conservative chars≥bytes);
   project/venue/node caps relaxed bytes→code points with G1 tests updated;
   scene-name freeze invariant tested.
9. **Wire:** `signalNode.dspPresetRef` persisted and restored; legacy docs
   without the key open (tolerant codec); `""`-means-none wire test passes;
   **ref-existence enforced in `sf_graph_validate` + `sf_project_health_check`
   (dangling-ref fixture asserts error); native validator type-checks
   `dspPresetRef` string|null (native/Python parity, adversarial fixture)**;
   chain ORDER off-wire; `data` body unformalized; no preset authoring.
10. **Depth gate:** `checked_parse()` = 8 MiB byte cap + `scan_json_depth()` at
    kMaxJsonDepth = 256 → `SF_E_SCHEMA` + distinct `last_error`, applied to
    **all 4 raw-JSON entry points** (`from_json`, `open_from_path`,
    `sf_validate_project_json`, `sf_migrate_json`); scanner contract
    pre-rejects ⊆ nlohmann rejects (differential fuzz); unterminated-string
    has its own error text; third-party nlohmann fork disposition =
    **BLOCKED, documented** (G3-1).
11. **UI:** `DspChainScreen` real editor + `SceneEditorScreen` scene-name split
    + `DirtyField` on the new fields only; zero `NativeBridge.*` in
    composables; ViewModels g1.1-hardened; export grep 26.
12. **Docs:** this plan + `docs/RELEASE_NOTES_G3.md` checked in; §10
    dispositions resolved; tag `g3-complete`.

**Exit artifact:** tag `g3-complete`; release notes attach the law spec
(additive keys), the runner contract summary, and the §10 residuals table.

---

## 9. Risks

| # | Risk | Sev | Mitigation |
|---|---|---|---|
| R1 | ASan dead under proot (env) | MED (env) | UBSan is the sanctioned sanitizer; runner races bounded by explicit atomics + join-before-destroy tests; Android stays static-only (G2-4) |
| R2 | Runner threaded races (start/destroy/mutate) | HIGH | Ownership guards at ABI boundary (fast-reject atomic), destroy-guard, C7/C8 contract, lifecycle + deadlock-bounded tests (R4 reviews) |
| R3 | Multi-source law re-definition surprises consumers | MED | Additive keys only; single-source numbers byte-identical (tests); release-note callout; `powerGainLin` brackets the true level |
| R4 | UTF-8 fail-open counting lets hostile names pass | LOW/MED | Documented semantics + byte ceiling (800/256) keeps wire bounded; security review of the table (D5 pointer) |
| R5 | Idle-poll CPU cost on the runner | LOW | yield + ~1 ms sleep when empty; batch drain (64/quantum); measurable in tests |
| R6 | Golden/discipline slip on the wire refresh | MED | P5 regen + both pins in one commit; drift diff in §7.5 gate; G2-1 watch-item |
| R7 | Kotlin-only evidence for UI (no host runtime) | MED (env) | Static review checklist per G2 §7.4; export-grep parity; pure-view rule enforced |
| R8 | Law/kernels timing or FP nuance on ARM target | LOW (env) | Deterministic-per-platform only (bit-exact NOT claimed, G3-3); G2-4 device lane when available |

---

## 10. Residuals & Open Reviews

### 10.1 G1 residuals — disposition this gate

| G1 residual | Disposition in G3 | Rationale |
|---|---|---|
| **JSON parse depth cap** | **Mitigation ships; third-party patch BLOCKED.** `scan_json_depth()` iterative pre-parse gate (kMaxJsonDepth 256 → `SF_E_SCHEMA`) lands beside the G2 byte cap; the nlohmann fork itself has no available patch in this environment → documented **BLOCKED** (G3-1). | Bounds the stack-exhaustion exposure without waiting on a vendor fix; parser behavior untouched. |
| **Rename cap is bytes, not chars** | **FIXED (additive).** `sf_scene_rename` ships char-capped (≤200 cp / ≤800 B); `sf_project_rename`/`sf_venue_rename`/node-label caps relaxed bytes→code points; G1 tests/docs updated; release-notes behavioral-callout. | The G3 name-model work the G2 §10.1 row pointed at; relaxation-only keeps the ABI additive. |
| **Reseed-on-refresh discards unapplied field typing** | **Mitigated for new surfaces only.** `DirtyField<T>` (D7) covers the new scene-name + DSP-chain fields; existing G2 screens keep the documented trade. | Bounded dirty layer without reopening g1.1 race contract. |

### 10.2 G1/G2 TODO carry-forward

- `sf_scene_rename` + scene-name freeze tracking: **landed in G3 P1** (G2 §10.2
  resolved). Multi-scene documents themselves remain deferred (scene is a
  frozen-named singleton; the mutator is the name-model piece).

### 10.3 G2 residuals — disposition this gate

| # | Residual | Disposition |
|---|---|---|
| G2-1 | Schema↔native action-table lockstep watch-item | **STANDS.** P5's refresh updates the golden + both pins in one commit; pytest enum test is the guard. |
| G2-2 | 176-B ring slot caps payloads | **CLOSED** (D4): STOP/EVALUATE fit; blob payloads → G3-2 side-channel. |
| G2-3 | `apply_batch` non-atomic stop-on-first-error | **STANDS** (contract); runner batches inherit the same semantics (documented). |
| G2-4 | Android static-only; ARM/Termux timing untested | **STANDS** (env) — CI device lane when available. |
| G2-5 | Desk estimate vs DSP law | **CLOSED** (D1): the law lands in `sfdsp`, shared by `evaluate_mixer` and the render harness; the G2 JSON is its stable input. |

### 10.4 New G3-discovered residuals (documented, non-blocking)

| # | Residual | Sev | Disposition |
|---|---|---|---|
| G3-1 | nlohmann JSON **depth** fix = third-party fork — **BLOCKED** (no patch in env); the pre-parse scanner is the mitigation | MED (external) | Documented; revisit if a vendor patch appears or the scanner proves insufficient |
| G3-2 | Command payloads > 168 B (audio-config blobs) have no queue path | LOW | Side-channel design (future gate); D4 keeps the slot constant |
| G3-3 | Bit-exact cross-platform DSP equality NOT claimed (per-platform determinism only) | LOW | Toleranced tests; G2-4 device lane for target verification |
| G3-4 | Preset *authoring* deferred — editor attaches/replaces/clears existing envelopes only | LOW | `dspPresets[].data` unformalized; authoring is a future gate |
| G3-5 | No actual audio device callback — the runner is a queue-consumer lane, not an audio engine | LOW (by design) | G4 wires a real callback to runner/harness; `app/platform/audio/*` unchanged |
| G3-6 | Chain ORDER off-wire (derived from topo order); deterministic only while the graph is acyclic | LOW | Mutation-time cycle rejection (G2) makes order unambiguous |
| G3-7 | UTF-8 fail-open counting semantics (hostile/broken input) | LOW | Documented + unit-tested; byte ceilings bound the wire |

### 10.5 Open reviews — pre-review list (@oracle + @security-reviewer)

Highest-risk decisions needing adversarial review **before P1**:

| # | Decision (ref) | Why high-risk | Asked to verify |
|---|---|---|---|
| R-A | **Mixing-law re-definition** — `peakGainLin` max→coherent-sum re-label + new `powerGainLin`/`headroomDb` (D1) | Changes semantics of a shipped query output, even if additive + single-source-invariant | Single-source invariance proof; empty-route `headroomDb` choice; `clipped` refactor equivalence |
| R-B | **Runner ownership/destroy guard** — active-runner fast-reject, destroy guard, cmd-7 single-slot report (D3) | Thread-safety heart of the gate; a missed internal mutator path breaks C3 | Race analysis of start/stop/join/destroy orderings; audit of every internal mutator site for the busy-guard |
| R-C | **`dspPresetRef` on-wire schema refresh + golden discipline** (D6) | Wire refresh regen is a reviewed event; a slip silently weakens the drift guard | P5 commit contains regen + both pins; codec null↔"" symmetry; legacy-doc tolerance |
| R-D | **UTF-8 fail-open char caps + `scene.update` audit reuse** (D5) | Security-adjacent semantics (hostile names, wire bound) and audit-integrity | `utf8_char_count` table incl. truncated/overlong; freeze invariant; audit reuse doesn't change `scene.update` semantics |
| R-E | **Depth scanner correctness** — string/escape-aware iterative pre-parse (D7/P7) | Parser-adjacent code is classic false-positive/false-negative territory | Escape/Unicode/string cases; scanner never diverges from nlohmann on accept/reject |

**Verdicts — recorded 2026-09-13 after adversarial review (G2 §10.4 style);
no phase starts on a HIGH finding.**

### 10.5.1 Security review verdicts (@security-reviewer, attempt 1/3)

| # | Finding | Sev | Resolution (folded into plan) |
|---|---|---|---|
| SEC-G3-1 | `sf_graph_apply_batch` = 14th mutating entry, unguarded; flag bypass TOCTOU | **HIGH** | **RESOLVED** — `apply_batch_impl` split (public busy-reject→forward; runner calls impl); `command_queue.cpp` KEEP→EDIT; P4a acceptance + test (P4a §6) |
| SEC-G3-2 | `sf_project_destroy` is `void` — cannot return `SF_E_IO` | MED | **RESOLVED** — keep void; `set_handle_error("project.destroy: queue runner active")` + skip free; second destroy after stop+join mandatory; header doc (D3-amd, P4b) |
| SEC-G3-3 | stop→destroy TOCTOU (flag-clear ordering) | MED | **RESOLVED** — lifecycle state machine `{IDLE,RUNNING,STOPPING,STOPPED}`; RUNNING→STOPPED only by runner epilogue; destroy rejects unless STOPPED(joined) (D3-amd, P4a/P4b) |
| SEC-G3-4 | Read entry points unguarded → data race with runner; `lastError` plain string | MED | **RESOLVED** — read-path guards (reject, except `sf_last_error`/destroy); runner never writes `proj->lastError` (R-B(c), P4b) |
| SEC-G3-5 | Runner handle lifetime unowned; `last_report` truncation silent | LOW | **RESOLVED** — 6th export `sf_queue_runner_destroy`; one-shot (start from IDLE only); `"_truncated": true` sentinel (ORC-3 fold, P4a) |
| SEC-G3-7 | Depth gate bypass: `sf_validate_project_json` + `sf_migrate_json` ungated | MED | **RESOLVED** — `checked_parse()` = 8 MiB cap + depth gate on **all 4** raw-JSON entries; `migration.cpp` + `project.cpp` added to P7 (P7 §6) |
| SEC-G3-8 | Scanner divergence risk; under-count is the bypass direction | MED | **RESOLVED** — strict one-escape-char state machine; distinct `last_error` (depth vs unterminated); differential fuzz scanner⊆nlohmann (P7, test_json_depth) |
| SEC-G3-9 | Dangling `dspPresetRef` persists through load (mutation-time check ≠ load) | MED | **RESOLVED** — ref-existence in `sf_graph_validate` + `sf_project_health_check`; load tolerant; `""`-means-none test (D6-amd, P5) |
| SEC-G3-10 | Native validator ↔ Python drift on the new property | LOW | **RESOLVED** — native type-check `dspPresetRef` string\|null; adversarial number-typed fixture rejected both hemispheres (D6-amd, P5) |

### 10.5.2 Oracle verdicts (@oracle, attempt 1/3)

| # | Finding | Sev | Resolution (folded into plan) |
|---|---|---|---|
| R-A | Mixing-law re-definition — single-source invariance verified against actual tests; `headroomDb` null-on-empty is *required* (log10(0)=−∞ dumps undefined); `clipped` ≡ G2 predicate; pan omission safe-directional (per-channel bound) | OK | Insert new keys alphabetically in per-output dump; law spec states `number\|null` headroom + per-channel-bound property (D1 verdict note) |
| R-B | Runner ownership sound once amended; read-path needs a decision | OK (cond.) | Read-path = **reject** (not contract); `save_to_path` is a mutator; P4a/P4b split (D3-amd) |
| R-C | Wire design coherent; drift/digest/codec claims all verified against code | OK | D6-amd fold (SEC-G3-9/10 placement confirmed: `graph_abi.cpp:171` + `project.cpp:417-447`) |
| R-D | UTF-8 fail-open conservative (every char ≥1 byte) — no exploit; freeze test meaningful; `scene.update` reuse unambiguous (`detail` distinguishes) | OK | No plan change; semantics documented in law/name-model sections |
| R-E | Depth gate direction correct; wording hazard between "unterminated→pre-reject" and "malformed→nlohmann" | OK (cond.) | Contract stated as **pre-rejects ⊆ nlohmann rejects; accept-set equal**; distinct error texts (P7) |
| ORC-1 | State machine in `SfProject` breaks `sf_project_clone` copy-ctor (atomics) | MED | **RESOLVED** — explicit copy ctor: `doc`+`lastError` copied, runner state default (IDLE); clone-RUNNING → `SF_E_IO` (D3-amd, P4b) |
| ORC-2 | `SF_CMD_STOP` barrier semantics undefined inside a 64-batch buffer | MED | **RESOLVED** — drain stops filling at first STOP; `[0..k)` apply; behind stay queued; `ADD→STOP→ADD` test (P4a) |
| ORC-3 | `sf_queue_runner` lifetime/restart/truncation unspecified | MED | **RESOLVED** — `sf_queue_runner_destroy` (6th export); one-shot lifecycle; `"_truncated"` sentinel (P4a) |
| ORC-4 | Mechanical gaps: `Threads::Threads` link; `save_to_path`/`sf_scene_rename` in guard list; §2 flips | LOW | **RESOLVED** — folded into §2 + P4 file lists |

### 10.5.3 Gate verdict (2026-09-13)

Both reviews initial attempts; security gate was **BLOCKED pending amendment**
(SEC-G3-1 HIGH), oracle gate returned **approvable** (no HIGH; ORC-* all
MED/LOW). All findings are plan-text amendments — **all folded above and in the
phases** — no architecture rework. **Effective verdict: APPROVED — P0 commits
this amended plan; no phase starts on an open HIGH finding.**

---

*End of PLAN_G3.md*