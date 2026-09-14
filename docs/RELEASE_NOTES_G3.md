# SoundForge — Release Notes G3

**Gate:** G3 — Full Audio Gate: DSP Engine, Mixing Law, RT Queue-Drain Lane,
DspChain Editor, Name Model, Depth Cap. **Tag:** `g3-complete`.
**Engine version:** `0.1.0-g3`. **Schema:** `project_v2` (schemaVersion **2**,
one additive refresh to v2.2 during P5 — see below).

## Deliverables

The static routing-desk estimate becomes a *real, host-unit-testable DSP law*
(a `sfdsp` static library with coherent-power/headroom law + finite-block audio
kernels), `evaluate_mixer` reports that law as an additive superset of the G2
shape, the G2 command queue gains a *live RT queue-drain lane*
(`sf_queue_runner`) with single-owner ABI guards, `dspPresetRef` crosses the
wire under full golden discipline, the DSP-chain route becomes a real editor,
the G1 rename byte-cap residual and `sf_scene_rename` land as the name model,
and the JSON *depth* cap becomes a real pre-parse mitigation on all four
raw-JSON entries. All layered additively on the G0/G1/G2 shell.

### P1 — Name model + version stamp (`1436644`)
- `sf_scene_rename` (scene object rename; audit `scene.update`, `modifiedAt`
  bumped, name survives save/reopen; freeze invariant tested).
- `utf8_char_count` char-cap model (fail-open documented, conservative
  chars ≥ bytes): scene ≤200 cp / ≤800 B; project/venue ≤200 cp; node label
  ≤64 cp — G1 byte caps relaxed bytes→code points, G1 tests updated.
- Version stamp `0.1.0-g3` in `sf_version.h`, `native/CMakeLists.txt` default,
  both reconfigured caches, python `__version__` + `migrate.py` `ENGINE_VERSION`;
  the **missed G2 bump** (still `-g1` in the tree) documented.

### P2 — `sfdsp` static library (`c0bd9b8`)
- `dsp/CMakeLists.txt` INTERFACE→STATIC; `law.cpp` + `kernels.cpp` (NEW).
- Law: coherent peak sum, incoherent power sum (`√Σg²`), headroom, clip gate.
- Kernels: `apply_gain`, `apply_pan` (equal-power, L²+R²=1), `apply_gate`,
  `mix_bus`, `soft_limit` (finite for ±1e30, exact ceiling, no NaN/Inf,
  deterministic ≤1e-12).
- `test_dsp_law.cpp` + `test_dsp_kernels.cpp` (NEW); nothing wired yet.

### P3 — Law integration + chain-render harness (`48ad1c0`)
- `routing.cpp` `evaluate_mixer` aggregates via `sfcore::dsp::merge_law` —
  additive keys only (`peakGainLin`, `powerGainLin`, `headroomDb`; `clipped`
  law-derived); single-source numbers byte-identical (G2 tests pass unmodified).
- `dsp_render.cpp` (NEW) finite-block chain harness compiled into `sfgraph`;
  `test_dsp_render.cpp` (NEW) + `test_graph_mixer.cpp` (EDIT, multi-source).

### P4a — Runner module + state machine + contract (`3cc98e5`)
- `sf_queue_runner.h` (6 exports: create/start/stop/join/last_report/**destroy**),
  `command_queue_thread.cpp` (NEW).
- Lifecycle state machine `{IDLE,RUNNING,STOPPING,STOPPED}` (RUNNING→STOPPED
  only by runner epilogue); explicit `SfProject` copy ctor (runner state
  default IDLE) for `sf_project_clone`.
- `apply_batch_impl` split: public `sf_graph_apply_batch` busy-rejects then
  forwards; runner calls impl. `SF_CMD_STOP` barrier semantics
  (`[0..k)` apply, behind stay queued); `"_truncated": true` sentinel.
- `test_queue_runner.cpp` (NEW), `test_cmd_queue.cpp` (EDIT).

### P4b — Guard sweep (`9d60e85`)
- All 14 mutating entries + all `doc`-reading entries reject
  `SF_E_IO "project.busy: queue runner active"` while RUNNING — except
  `sf_last_error` and `sf_project_destroy`.
- Destroy guard (void-safe: `set_handle_error("project.destroy: queue runner
  active")` + skip free); clone-during-RUNNING `SF_E_IO`; runner never writes
  `proj->lastError`. `test_queue_runner.cpp` (EDIT — guard cases).

### P5 — `dspPresetRef` on the wire (`06ed626`)
- Schema v2.2 additive refresh: `signalNode.dspPresetRef` (`string|null`);
  `json_codec.cpp` write/read (`""` ↔ null); `schema.cpp` validator type-check.
- `schema_golden_v2.json` REGEN **byte-identical** + both pytest SHA-256 pins
  in the same commit; `project_dspchain_v2.json` fixture (NEW).
- Ref-existence enforced in `sf_graph_validate` + `sf_project_health_check`
  (dangling-ref fixture asserts error; `""`-means-none wire test passes);
  legacy docs without the key open cleanly. `test_graph_roundtrip.cpp`,
  `test_schema_validate.cpp`, `SignalKt.kt` (EDIT).

### P6 — DspChain editor + scene-name field + dirty flags (`d4bec4c`)
- `DspChainKt.kt`, `DspChainViewModel.kt`, `common/DirtyField.kt` (NEW);
  `DspChainScreen.kt` real editor (nodes + preset chips attach/replace/clear
  over existing `dspPresets` envelopes only, inline `lastEditError`),
  `SceneEditorScreen.kt` gains a distinct "Scene name" `DirtyField`
  (`sf_scene_rename`) beside the project-rename field.
- `jni_bridge.cpp` +`sceneRename` → **26** exports; `NativeBridge.kt` mirror.
- **Zero `NativeBridge.*` in composables**; ViewModels own native work under
  `nativeMutex`; `DirtyField.seed` never clobbers a dirty edit.

### P7 — JSON depth pre-parse gate (`7891643`)
- `checked_parse()` = 8 MiB byte cap + `scan_json_depth()` at
  `kMaxJsonDepth = 256` on **all 4 raw-JSON entry points** (`from_json`,
  `open_from_path`, `sf_validate_project_json`, `sf_migrate_json`).
- String/escape-aware strict one-escape-char iterative scanner; distinct
  `last_error` for depth vs unterminated string.
- `test_json_depth.cpp` (NEW) incl. differential fuzz
  scanner-verdict ⊆ nlohmann-outcome; third-party nlohmann **fork disposition
  = BLOCKED, documented** (G3-1). `test_schema_validate.cpp` (EDIT).

### P8 — Docs + DoD sweep + tag (this commit)
- `docs/RELEASE_NOTES_G3.md` (NEW) + `docs/PLAN_G3.md` finalized (all phases
  checked, gate status final); full §7.5 suite re-run and recorded below; tag
  `g3-complete`.

## Behavior callouts (consumers)

- **Mixing law is additive.** `evaluate_mixer` per-output JSON gains
  `powerGainLin` (√Σg², incoherent level) and `headroomDb` (`number|null`;
  `null` on no routes — `log10(0)` is undefined); `peakGainLin` is now
  documented as the **coherent worst case** (Σ|g|, unchanged for single-source
  graphs); `clipped ⇔ headroomDb < 0` (≡ the G2 `peak > 1.0` predicate).
  Every G2 key/value on single-source graphs is byte-identical.
- **Rename caps relaxed bytes→code points.** `sf_scene_rename` (≤200 cp /
  ≤800 B), project/venue (≤200 cp), node label (≤64 cp). A multibyte name that
  is > N bytes but ≤ N code points is now accepted (relaxation only). Invalid
  UTF-8 counts conservatively (chars ≥ bytes), so hostile input stays bounded
  by the byte ceilings.
- **Depth gate.** `kMaxJsonDepth = 256`, `checked_parse()` applied to all four
  raw-JSON entries; 256 accepted / 257 rejected with `SF_E_SCHEMA` and a
  distinct `last_error`; unterminated strings have their own error text.
  Malformed-but-shallow input is still decided by nlohmann — the scanner never
  re-implements the parser and never changes an accept.
- **`sceneRename` JNI (25 → 26).** `sf_scene_rename` passthrough with the same
  try/catch/`log_boundary_exception` guard as G1; one Kotlin mirror per
  external fun.
- **DspChain editor + `DirtyField`.** The G0 placeholder is a pure-view editor
  (zero `NativeBridge.*` in composables); `DirtyField<T>` tracks only the new
  scene-name + preset fields, seeds from `UiState.Ready` only, and never
  clobbers an in-progress dirty edit (document stays authoritative).
- **Runner lifecycle.** `sf_queue_runner` is one-shot
  (`IDLE→RUNNING`; start from STOPPED rejected); `destroy` requires
  IDLE/STOPPED (auto-joins a started-but-unjoined thread); while RUNNING every
  doc-touching mutator/read rejects `SF_E_IO` except `sf_last_error` and
  destroy; the runner never writes `proj->lastError`. **JNI never starts a
  runner** — reads issued in the RUNNING state are a contract violation (host
  tests only).

## Verification (host, PLAN_G3 §7.5)

| Check | Result | Note |
|---|---|---|
| `cmake --build native/build && ctest` | **232/232** | plan expected ~174 (119 + ≈55); the gate grew with per-phase extensions |
| UBSan build `ctest` (`native/build-asan`) | **232/232** | `-fsanitize=undefined -fno-sanitize-recover=all` — sanitizer substitute (ASan blocked by proot) |
| `pytest tests/python_tests -q` | **12 passed** | plan expected ~11; actual growth landed at 12 |
| drift diff canonical ↔ golden | **empty** | `project_schema.json` byte-identical to `schema_golden_v2.json` |
| JNI export grep | **26** | one per `NativeBridge` external fun (25 + `sceneRename`) |
| `SF_BUILD_VERSION` in both caches | **`0.1.0-g3`** | `native/build` + `native/build-asan` reconfigured |
| `ctest -R Version` | **4/4** | `sf_engine_version() == 0.1.0-g3` |
| `git status --short` before commit | **clean** | no non-docs changes |

Android: static-only in this container (no SDK/NDK) — the documented G0/G1/G2
limitation (G2-4). Evidence is export-grep parity (26), the No-NativeBridge
gate (zero `NativeBridge.*` references in composables — only comments; all
native work through ViewModels), `DirtyField` static review, and the
`PLAN_G3` §7.4 static checklist.

## Commits

| Commit | Phase |
|---|---|
| `ac61711` | plan(g3): reviewed PLAN_G3.md (adversarial amendments folded) |
| `1436644` | P1 — name model + `0.1.0-g3` stamp |
| `c0bd9b8` | P2 — `sfdsp` static library (law + kernels) |
| `48ad1c0` | P3 — law integration + render harness |
| `3cc98e5` | P4a — queue runner + lifecycle + `apply_batch_impl` split |
| `9d60e85` | P4b — guard sweep (single-owner enforcement) |
| `06ed626` | P5 — `dspPresetRef` on wire (schema v2.2 + golden + pins) |
| `d4bec4c` | P6 — DspChain editor + scene-name field + dirty flags (JNI 26) |
| `7891643` | P7 — depth gate on all 4 JSON entries |
| (this) | P8 — docs + DoD sweep + tag |

## Residuals (PLAN_G3 §10)

- **§10.1 G1 residuals:** depth cap → *mitigation ships, third-party patch
  BLOCKED* (G3-1); rename-cap-bytes → *FIXED (additive)*; reseed-on-refresh →
  *mitigated for new surfaces only* via `DirtyField`.
- **§10.2 G1/G2 TODO carry-forward:** `sf_scene_rename` + scene-name freeze
  tracking **landed in P1** (G2 §10.2 resolved); multi-scene documents remain
  deferred (scene is a frozen-named singleton).
- **§10.3 G2 residuals:** G2-1 *stands* (lockstep watch-item), G2-2 *CLOSED*
  (D4), G2-3 *stands* (batch contract), G2-4 *stands* (env — Android/ARM),
  G2-5 *CLOSED* (D1 — law lands in `sfdsp`).
- **§10.4 G3 residuals G3-1..G3-7:** all documented, non-blocking (third-party
  depth-fork BLOCKED; >168 B payload side-channel; no bit-exact cross-platform
  claim; preset authoring deferred; no real audio device callback; chain-order
  off-wire; UTF-8 fail-open semantics).
- **§10.5 adversarial review findings** (SEC-G3-1..10, ORC-1..4, R-A..R-E):
  all resolved/folded into the plan and shipped across P1/P4/P5/P7.

### Gate-review residuals carried

| # | Sev / status | Item | Disposition |
|---|---|---|---|
| ORC-P5-1 | LOW, deferred | Adversarial loop on the wire-ref refresh (fuzz the `dspPresetRef` codec/validator beyond the fixed adversarial fixture) | Optional hardening, not implemented; the fixed fixtures + native/Python parity tests are the shipped guard |
| ORC-P6-2 | LOW, accepted | `DirtyField` commits after handoff to the VM, not after VM accept; error-wipe is consistent with the G1 pattern | Accepted at gate review; matches the pre-existing G1 dirty/commit posture (document is authoritative) |
| ORC-P6-3 | LOW, accepted | A failed scene-name commit cannot surface the native message (falls back to the generic/local error) | Accepted; pre-existing G1 pattern, not G3-introduced |
| ORC-P6-4 | LOW, fixed pre-commit | Trailing newline in `DspChainScreen.kt` | Fixed before the P6 commit (`od` confirms a single terminating `\n`) |
| ORC-P7-8 | LOW, wording | `sf_internal.hpp:392-394` comment claims depth pre-rejects are "a subset of nlohmann's rejections" — strictly, the depth trip is a range pre-reject, not an nlohmann rejection | **Deferred (comment-only, out of P8 write scope — `native/`)**. Semantics are correct: the depth trip is the sanctioned intentional exception (PLAN_G3 §10.1). Revisit as a comment-only edit in a future native touch |
| ORC-P7-9 | LOW, latent test convention | `tree_depth()` helper seeds an object/array frame at depth 1 while the scanner's structural depth convention differs by an off-by-one frame origin | **Doc-level note only** (test convention, not a product defect); no test edit (tests/ out of scope). `tree_depth`'s comment documents the `depth()` convention and differential assertions still hold |
| ORC-P7-10 | INFO | One `build-asan` `ProjectIoTest` flake observed at `-j4`, not P7-caused | Recorded; the full §7.5 suite above ran clean (232/232 UBSan) — transient, not reproduced |

## Definition of Done (PLAN_G3 §8)

| # | Item | Result |
|---|---|---|
| 1 | Builds green on `native/build` **and** `native/build-asan` (UBSan); pytest green; JNI grep **26** | **PASS** — 232/232, 232/232, 12 pytest, grep 26 |
| 2 | No drift: canonical ≡ `schema_golden_v2.json`; both SHA-256 pins in the P5 commit; v1 untouched; schemaVersion **2** | **PASS** — drift diff empty; pins co-committed in `06ed626`; schemaVersion 2 |
| 3 | Version stamp `0.1.0-g3` in `sf_version.h`, CMake default, both caches; python `__version__` + `ENGINE_VERSION`; missed G2 bump documented | **PASS** — both caches reconfigured; `-R Version` 4/4 |
| 4 | DSP law: stable superset of G2 shape; existing single-source tests unmodified; `powerGainLin`/`headroomDb` added; `clipped` law-derived | **PASS** — `test_dsp_law` + `test_graph_mixer` (G2 cases untouched) |
| 5 | Kernels unit-tested (pan L²+R²=1, ceiling, no NaN/Inf, determinism); no new public C ABI / JNI beyond 26; `audio/*` stub untouched | **PASS** — `test_dsp_kernels`; grep 26; `audio/*` unmodified |
| 6 | Render harness proves law+kernels compose (host only) | **PASS** — `test_dsp_render` (8 cases) |
| 7 | Queue runner: C1–C4 unchanged, C5/C6 rewritten, C7/C8 added; lifecycle + guards + STOP barrier + clone + destroy export tested | **PASS** — `test_queue_runner` (23 cases incl. P4b guards) |
| 8 | Name model: `sf_scene_rename` + audit `scene.update`; char caps; freeze invariant | **PASS** — `test_name_model` + `SceneRename.*` |
| 9 | Wire: `dspPresetRef` persisted/restored; legacy tolerant; ref-existence enforced; native validator type-check; order off-wire | **PASS** — `DspPresetRef*` + roundtrip tests |
| 10 | Depth gate: `checked_parse()` on all 4 entries; scanner pre-rejects ⊆ nlohmann; distinct error texts; fork BLOCKED documented | **PASS** — `test_json_depth` (13 cases incl. differential fuzz) |
| 11 | UI: `DspChainScreen` editor + scene-name split + `DirtyField`; zero `NativeBridge.*` in composables; grep 26 | **PASS** (static) — export grep 26; composables comment-only references |
| 12 | Docs: plan + `RELEASE_NOTES_G3.md` checked in; §10 dispositions resolved; tag `g3-complete` | **PASS** — this document + finalized plan + tag |

## Environment limitations

- **ASan is proot-blocked** (shadow-region mapping CHECK at
  `sanitizer_allocator_primary64.h` init, per G2 P5). The sanctioned sanitizer
  substitute is the `native/build-asan` **UBSan** build
  (`-fsanitize=undefined -fno-sanitize-recover=all`); the full suite is green
  there. **TSan attempt recorded (G3-8):** `native/build-tsan` compiles the
  runner tests but the TSan runtime aborts at test discovery with
  `FATAL: ThreadSanitizer: unexpected memory mapping` — the same proot
  memory-map restriction that blocks ASan, so TSan is **BLOCKED** (environment,
  not a code finding). The runner's race safety is instead evidenced by the
  explicit atomics + lifecycle state machine + join-before-destroy and
  concurrent-mutator-hammer tests under UBSan; a TSan lane becomes available
  when a real device/CI environment is.
- **Android is static-only** in this container (no SDK/NDK, no `assembleDebug`,
  no logcat) — G2-4 stands. UI evidence is export-grep parity, the
  No-NativeBridge gate, and static review per PLAN_G3 §7.4; the Android runner
  lane is `NOT VERIFIED` by design.
- **G3-5:** there is no real audio-device callback — the runner is a
  queue-consumer lane, not a live audio engine; a G4 callback wires the device
  to the runner/harness.
