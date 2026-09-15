# SoundForge — Release Notes G4

**Gate:** G4 — Native Host-Testable Audio Engine: `sf_queue_runner` +
`render_chain`, Per-Channel True Peak, RenderPlan Seam. **Tag:** `g4-complete`.
**Engine version:** `0.1.0-g4`. **Schema:** `project_v2` (schemaVersion **2**,
**unchanged** — G4 adds zero wire surface).

## Deliverables

The G3 "render harness" (`sfcore::render_chain`) becomes a **real,
host-unit-testable native audio engine** (`sf_audio_engine`): a virtual device
callback driven by a deterministic synthetic clock (or an optional pacer
thread) renders the live signal graph that `sf_queue_runner` mutates, measured
per channel by a **4×-oversampled true-peak** meter, and the preallocation seam
at `dsp_render.cpp:27` is fulfilled by a compiled **`RenderPlan`** — zero
allocation on the hot path. The engine binds the G3 queue runner to the P3
snapshot store via a **lock-free 4-slot plan handoff** (writer = runner thread,
reader = tick/pacer thread; no doc access, no read-guard exemption). It is a
**host-only** surface: **no new JNI** (grep stays 26), **no `app/**` change**,
**no schema/wire change**, **no new `SF_E_*`** (7 codes, all reused). All layered
additively on the G0–G3 shell.

### P1 — `RenderPlan` + zero-alloc `render_chain_planned` (`0864449`)
- `render_plan.{hpp,cpp}` (NEW, in **`src/graph/`** — R-B amendment: `sfdsp`
  stays a zero-dependency leaf) — preallocated per-node `AudioBlock` pool
  (the scratch that makes the hot path allocation-free), topo-order node list,
  precomputed gain/pan/exclusion flags, preds index lists in edge document
  order (preserves G3 float accumulation order).
- `dsp_render.cpp` — `render_chain` re-expressed as **compile + execute**
  (still literally `dsp_render.cpp:40-45`); every G3 `Render.*` case passes
  unmodified. `graph_internal.hpp` + `graph/CMakeLists.txt` (EDIT).
- `test_render_plan.cpp` (NEW, 10 cases): planned == facade ≤1e-12, DC −6 dB →
  half amplitude, mute/solo exclusion, empty-graph identity, unknown-target
  silence+false, pan==0 passthrough, **zero-allocation** (source scan + shared
  counting allocator), compile-failure path safe.
- **ORC-G4-02 (discharged P6):** `test_dsp_render.cpp` was declared EDIT but
  is **intentionally unchanged** — see §Finding discharge.

### P2 — `sfmeasure` true-peak meter (`b3ab00e`)
- `true_peak.{hpp,cpp}` (NEW) — per-channel **4× oversampled polyphase FIR**
  (Kaiser-windowed sinc, M=24, β=20, phase lens {25,24,24,24}, K=24), raw
  `float* const*` channels (B-3: leaf — no `AudioBlock`/`sfdsp` include),
  K-sample lookback tail, **non-decaying latch**, `clipped ⇔ latch ≥ 1.0`
  (0 dBFS contract, not −1 dBTP — G4-2), ordered finite compare (NaN/Inf
  cannot poison the latch). Hot path: no locks, no malloc, no SoundForge
  target. `sfmeasurement` → **`sfmeasure`** STATIC (B-1).
- `test_true_peak.cpp` (NEW, 16 cases): fs/4 + π/4 → **+3.01030 dB ±0.1**, DC
  unity ±1e-4, multi-block hold, latch monotone, reset, silence, full-scale DC
  clips / steady-state fs/4 does not spuriously clip (R-C passband bound),
  ±1e30 finite, determinism ≤1e-12, per-channel independence, block-size
  invariance, NaN/Inf no-poison, allocation counter.
- `tests/unit/alloc_counter.{hpp,cpp}` (NEW) — shared counting allocator
  extracted for `test_render_plan.cpp` + `test_true_peak.cpp` (link fix).

### P3 — Plan snapshot store + runner publish hook (`fa7910b`)
- `snapshot.{hpp,cpp}` (NEW) — **4-slot** `{EMPTY,WRITING,READY,READING}`
  lock-free single-writer/single-reader handoff (R-A amendment: ≤1 WRITING + ≤2
  transient READING = 3 < 4 → publish is wait-free, `spin_count` asserted 0);
  `RenderPlan` **by value** per slot; monotone `seq`; **exception containment**
  (ORC-G4-03/SEC-G4-03): whole compile incl. `topological_order` wrapped in
  `catch(...)`, slot restored `EMPTY` first, handlers never allocate, last
  valid plan stays current, never strands WRITING (4×-OOM exhaustion pin).
- `command_queue_thread.cpp` — internal (non-exported) `set_observer`; fires
  after `apply_batch_impl` only when `applied>0`; empty drain / `SF_CMD_STOP` /
  cmd 7 never republish; clean detach before destroy; runner never writes
  `proj->lastError` (SEC-G3-4).
- `test_snapshot.cpp` (NEW, 14 cases) incl. real-store-as-observer runner
  integration (ORC-G4P3-02) and oldest-reclaim multiset `{2,3,4,5}`
  (ORC-G4P3-03); 20k-plan stress, no torn/no-lost/spin 0.

### P4 — `sf_audio_engine` host audio engine (`1219dd3`)
- `sf_audio_engine.h` (NEW, 178 lines) + `audio_engine.cpp` (NEW, 673 lines) —
  **11 host-only `extern "C"` exports** (`create/destroy/configure/set_output/
  start/stop/join/tick/last_report/meter_json/reset_meters`), opaque handle,
  `sf_audio_engine_io_t` read/write callback protocol, config struct,
  `SF_AUDIO_ENGINE_PACE`.
- **Security-gate amendments landed here:** SEC-G4-01 `meter_json` contract
  (NULL `e`/`buf`/`cap==0` → `SF_E_INVALID_ARG`; too-small `cap>0` →
  `SF_E_NOMEM` + `buf[0]='\0'`, never truncated; bounded `std::to_chars`
  serialize, locale-independent; 9-key JSON `{channels, oversample,
  blocksRendered, truePeakLinear[2], truePeakDb[2], clipped[2], planValid}`);
  SEC-G4-02 single engine mutex around process/latch/reset + meter reads (no
  kernel locks); SEC-G4-05 ordered start gate + snapshot-#0 rollback (stop+join,
  `SF_E_IO`, restore CREATED; ORC-G4P4-01 rollback-store null fix); SEC-G4-06
  destroy-mandatory-first documented; SEC-G4-10 `destroy(NULL)` no-op + claim
  release ordering; R-D destroy order (pacer → runner stop/join → observer
  detach → runner destroy → free store → free engine → release claim).
- **ORC-G4-06:** `set_output("")`/NULL = **valid silence** (`SF_OK`, not an
  error); unknown target compiles `out_index==-1` → silence+false (G3-exact).
- `last_report` passthrough byte-identical; `test_audio_engine.cpp` (NEW, 32
  cases at the P4 commit, 35 in HEAD incl. the 3 P5 e2e tests); `test_render_plan.cpp`
  (+direct planned-path + entry-bound tests from ORC-G4P4-02/03).

### P5 — Engine + runner end-to-end (`0b1ecc1`)
- `test_audio_engine.cpp` (+3 e2e cases, 198 lines): `source(-6 dB) → output`
  tick → 0.501187 output, `clipped false`; `SF_CMD_SET_MIXER(0 dB)` enqueued
  while RUNNING → drain (STOP barrier + runnerState poll) → next tick 1.0
  (latch rings 1.062817, settles 1.0±1e-3 `clipped true`); mute mutation →
  exact-zero silence with the latch **holding** (tail ring 0.564303, then
  bit-identical on a second silent block — non-decaying); external
  doc-touching reads reject `SF_E_IO "project.busy: queue runner active"` while
  RUNNING and the handle is synchronously usable after stop+join (G3 C8);
  schemaVersion 2, no `project.migrate` audit entry.
- Race-free by construction (SPSC FIFO + publish sequenced before the STOPPED
  release-store, `command_queue_thread.cpp:176-205`); oracle P5 = **PASS**
  (numerics bit-exact vs independent float32 sim; one LOW non-blocking
  ORC-G4P5-01 documented).

### P6 — Version stamp + DoD sweep + release notes (this commit)
- Version `0.1.0-g4` stamped + both caches reconfigured; full §7.5 suite
  re-run and recorded below; docs amendments discharge ORC-G4-01/02/04/05;
  this document.

## Engine ABI (host, `sf_audio_engine.h`)

11 host-only exports (no JNI, no Kotlin mirror; grep stays **26**): `create`,
`destroy`, `configure`, `set_output`, `start`, `stop`, `join`, `tick`,
`last_report`, `meter_json`, `reset_meters`. Lifecycle
`{CREATED,STARTING,RUNNING,STOPPING,STOPPED}`; one-shot start; destroy requires
not-RUNNING/not-STOPPING (auto-joins pacer then runner). `tick` is
deterministic (no wall clock): `io.read` NULL/non-OK → zeros (silence),
`io.write` NULL/non-OK → block skipped. The `float* const*` IO struct is a
**host protocol** — never wired to JNI (G2 §1.3 preserved).

## True-peak spec (as shipped)

4× oversampled Kaiser-windowed-sinc polyphase (M=24, β=20, L=97,
phase lenses {25,24,24,24}, K=24); per-channel; non-decaying latch since
`sf_audio_engine_reset_meters`; `clipped ⇔ latch ≥ 1.0` (0 dBFS); fs/4 + π/4 →
+3.01030 dB ±0.1; **not** BS.1770-certified (G4-2 — documented approximation).

## Snapshot protocol (as shipped)

`PlanSnapshotStore`, 4 slots, per-slot state `{EMPTY,WRITING,READY,READING}` +
monotone `seq`; single writer (runner thread, after each applied batch) /
single reader (tick/pacer thread); publish wait-free (4th-slot guarantee);
acquire = largest-`seq` READY → CAS READING with post-CAS authoritative `seq`
re-read (stale-scan race fix); reader never touches `proj->doc`.

## Verification (host, PLAN_G4 §7.5)

| Check | Result | Note |
|---|---|---|
| `cmake --build native/build && ctest` | **308 registered / 307 passed, 1 skipped** | plan expected ~277; landed 308 (76 net-new vs G3's 232); 1 skip = `AudioEngine.MeterJsonIsLocaleIndependent` (GTEST_SKIP — `de_DE.UTF-8` absent under proot, present since P4) |
| UBSan build `ctest` (`native/build-asan`) | **308 registered / 307 passed, 1 skipped** | `-fsanitize=undefined -fno-sanitize-recover=all` — sanitizer substitute (ASan blocked by proot); same 1 locale skip |
| `pytest tests/python_tests -q` | **12 passed** | unchanged (version string only) |
| drift diff canonical ↔ golden | **empty** | `project_schema.json` byte-identical to `schema_golden_v2.json` |
| JNI export grep | **26** | `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` — G4 adds **no** JNI |
| `SF_BUILD_VERSION` in both caches | **`0.1.0-g4`** | `native/build` + `native/build-asan` reconfigured |
| `ctest -R Version` | **4/4** | `sf_engine_version() == 0.1.0-g4`; `SF_ENGINE_VERSION` macro suffix `-g4` |
| schemaVersion | **2** | `SF_SCHEMA_VERSION` untouched; no migration; no `project.migrate` |
| New `SF_E_*` codes | **none** | `sf_types.h` byte-identical to G3 (`sf_types.h:15-21`, 7 codes) |
| `git status --short` before commit | **4 version files + docs only** | no source/CMake edits beyond the version bump |

Android: static-only in this container (no SDK/NDK) — the documented G0–G3
limitation (G2-4). Evidence is export-grep parity (26), the No-JNI rule,
`app/**` byte-identical to the G3 baseline, and the PLAN_G4 §7.4 static
checklist. **Android device lane NOT VERIFIED by design** (G4-1).

**Known comparison-pair limitation (ORC-G4-05):** `RenderPlan.
ExecuteMatchesG3RenderChainBitForBit` compares the planned path against the
`render_chain` facade — which is now literally `compile_render_plan` +
`render_chain_planned` (`dsp_render.cpp:40-45`), i.e. **the same path twice**.
It is retained as the G3-contract regression (the facade is what G3 callers
use). The real cross-path proof is the **8 untouched `test_dsp_render`
`Render.*` G3 cases** (bit-exact G3 expectations vs the facade) plus the direct
planned value tests in `test_render_plan.cpp`.

## Commits

| Commit | Phase |
|---|---|
| `ed98bd6` | plan(g4): PLAN_G4.md — reviewed native audio engine + true peak + RenderPlan seam |
| `0864449` | P1 — `RenderPlan` preallocated pool + `render_chain_planned` (dsp_render seam) |
| `b3ab00e` | P2 — `sfmeasure` true-peak — 4× polyphase FIR + latch |
| `fa7910b` | P3 — plan snapshot store + runner publish hook |
| `ef078e4` | docs(g4): two-reviewer gate amendments (SEC-G4-01/02/05 + G4-8..10) |
| `1219dd3` | P4 — `sf_audio_engine` — host device callback + pacer + meter json (no new jni) |
| `0b1ecc1` | P5 — audio engine + queue runner end-to-end; true-peak hold across mutations |
| (this) | P6 — release notes + DoD sweep; version `0.1.0-g4` |

## Residuals (PLAN_G4 §10.1/§10.2)

- **§10.1 G3 residuals:** G3-5 **PARTIALLY CLOSED** (the host virtual device
  ships; a *real* AAudio/OpenSL device remains deferred → G4-1); G3-2
  (payload side-channel), G3-8 (TSan blocked — env), G3-3 (no bit-exact
  cross-platform claim), G3-4 (preset authoring), G3-7 (UTF-8 fail-open)
  **STAND**. Both SHA-256 golden pins untouched; v1 golden untouched.
- **§10.2 new G4 residuals G4-1..G4-10** (all documented, non-blocking):
  - G4-1 real Android device **LOW/by design**; G4-2 true peak is a 4× polyphase
    approximation, **not** BS.1770-certified (tolerances documented, revisited
    only if certification is required); G4-3 pacer best-effort (no RT
    priority/affinity under proot; deterministic tick is the authoritative
    path); G4-4 output target fixed pre-start (revisit with a live-control
    command); G4-5 plan memory ~4 KB/node, bounded by graph size, OOM
    contained (`planValid:false` + last-valid retention — SEC-G4-08); G4-6
    `std::atomic<double>` meter fields (per-field atomics + generation guard;
    assumption documented); G4-7 independent two-reviewer gate **RESOLVED at
    gate time** (ran out-of-band — oracle + security, verdicts in PLAN_G4
    §10.4); G4-8 `sf_project_destroy` cannot see an attached CREATED engine
    (SEC-G4-06 posture — engine destroy mandatory first, documented in
    `sf_audio_engine.h` + §4.5); G4-9 `alloc_counter` default-aligned only
    (SEC-G4-07 — precondition commented; test-only, no production target
    refs); G4-10 callback error/rollback paths **specified** (read non-OK →
    silence, write non-OK → skip, throwing callback contained, pacer-spawn
    failure → stop+join, `SF_E_IO`, restore CREATED — SEC-G4-09).
- **ORC-G4P5-01 (LOW, non-blocking):** the settle-phase `clipped:true` assertion
  rests on a 4.4e-8 coefficient margin — deterministic and bit-exact vs the
  independent float32 simulation; not flaky, documented only.

## Finding discharge (PLAN_G4 §10.4, all discharged at P6)

| Finding | Sev | Discharged in | Topic |
|---|---|---|---|
| ORC-G4-01 | LOW | **P6 (docs)** | `alloc_counter.{hpp,cpp}` added to plan §2/P1 file lists |
| ORC-G4-02 | LOW | **P6 (docs)** | `test_dsp_render.cpp` intentionally untouched — evidence = 8 untouched G3 `Render.*` cases through the unmodified facade (`dsp_render.cpp:40-45`) + planned value tests + structural operand-order identity |
| ORC-G4-03 (=SEC-G4-03) | MED | **P3** | publish-path compile containment (whole compile incl. `topological_order`) + cycle/OOM tests |
| ORC-G4-04 | LOW | **P6 (docs)** | self-loop `preds[i]==i` dead corner — comment-level disposition in §4.4 (rejected by `topological_order` as a cycle) |
| ORC-G4-05 | LOW | **P6 (docs)** | comparison-pair limitation stated in the evidence table (facade-vs-planned = same path twice) |
| ORC-G4-06 | LOW | **P4** | configured-empty output target = valid silence |
| SEC-G4-01 | MED | **P4** | `meter_json` buffer/error contract + locale independence |
| SEC-G4-02 | MED | **P4** | meter sync layer (engine mutex; no kernel locks) |
| SEC-G4-03 | MED | **P3** | = ORC-G4-03 (exception containment) |
| SEC-G4-04 | LOW | **P4** | `block.n > kBlockMaxSamples` entry guard + engine tick both-edge test |
| SEC-G4-05 | LOW (listed blocking) | **P4** | `start` ordered gate + snapshot-#0 rollback |
| SEC-G4-06 | LOW | **P4 (documented)** | destroy-mandatory-first posture (= G4-8) |
| SEC-G4-07 | LOW | **P1 (comment) / P6 (docs)** | `alloc_counter` default-alignment precondition (= G4-9) |
| SEC-G4-08 | LOW | **P6 (docs)** | plan memory bound (= G4-5) |
| SEC-G4-09 | LOW | **P4** | callback error/rollback paths (= G4-10) |
| SEC-G4-10 | LOW | **P4** | `destroy(NULL)` no-op + claim-release ordering |

## Definition of Done (PLAN_G4 §8)

| # | Item | Result |
|---|---|---|
| 1 | ctest green on `native/build` **and** `native/build-asan` (UBSan); pytest green; JNI export grep **26** | **PASS** — 308 registered / 307 passed + 1 locale skip, both builds; 12 pytest; grep 26 |
| 2 | No drift: `project_schema.json` ≡ `schema_golden_v2.json`; pins untouched; v1 untouched; schemaVersion **2**; no migration, no new key, no new `SF_E_*` | **PASS** — drift empty; pins untouched; schemaVersion 2; no `project.migrate`; `sf_types.h` byte-identical |
| 3 | RenderPlan seam: `render_chain` signature/behavior unchanged; `render_chain_planned` allocation-free (asserted); `dsp_render.cpp:27` promise fulfilled | **PASS** — all 8 G3 `Render.*` cases unmodified; alloc counter + source scan; G3 comment at `dsp_render.cpp:27` honored |
| 4 | True peak: per channel, 4× oversampled, fs/4 +3.01 dB proven, non-decaying latch + exact reset, `clipped` = latch ≥ 0 dBFS, separate `meter_json` | **PASS** — `test_true_peak` 16 cases; latch hold across silent blocks (P5); 9-key `meter_json` |
| 5 | Snapshot: 4-slot lock-free handoff; runner publishes per applied mutation; tick never touches `proj->doc`; no guard exemption; stress-tested | **PASS** — `test_snapshot` 14 cases + 20k stress + runner integration; single-reader protocol |
| 6 | Engine: 11 host-side exports; deterministic tick + optional pacer; lifecycle one-shot + misuse errors; `last_report` passthrough byte-identical; compile-failure safe | **PASS** — `test_audio_engine` 35 cases (29 + 3 e2e + 3 P4-amendments); D6 passthrough; `planValid:false` + silence |
| 7 | No new JNI / no Android: `app/**` untouched; export grep 26; `float* const*` host-only | **PASS** — `app/**` byte-identical to G3 baseline; grep 26; IO struct never exposed to JNI/Kotlin |
| 8 | Version stamp `0.1.0-g4` in `sf_version.h`, CMake default, both caches, python (`__version__`, `ENGINE_VERSION`) | **PASS** — `native/build` + `native/build-asan` reconfigured; `-R Version` 4/4; `SF_ENGINE_VERSION_SUFFIX` `-g4`; non-CMake fallback (`version.cpp:8`) also bumped |
| 9 | Concurrency: shutdown ordering tested; no deadlock; UBSan clean; TSan attempt recorded (BLOCKED → G3-8) | **PASS** — pacer/runner/destroy order under UBSan; TSan BLOCKED (env, G3-8) |
| 10 | Docs: plan + `RELEASE_NOTES_G4.md`; §10 dispositions resolved; tag `g4-complete` | **PASS (docs)** — this document + finalized plan; **tag applied by the orchestrator after the independent final audit (P6 commits docs only)** |

## Environment limitations

- **ASan is proot-blocked**; **UBSan** (`native/build-asan`,
  `-fsanitize=undefined -fno-sanitize-recover=all`) is the sanctioned
  sanitizer substitute — full suite green. **TSan remains BLOCKED** (G3-8:
  `unexpected memory mapping` under proot). The new concurrent surfaces
  (snapshot handoff, pacer thread) are instead bounded by the explicit atomics
  + single-writer/single-reader protocol + lifecycle tests + join-before-
  destroy, all green under UBSan.
- **Android is static-only** in this container (no SDK/NDK) — G2-4 stands.
  The engine is host-only by design; the Android device lane remains
  `NOT VERIFIED`.
- **Locale test skips:** `AudioEngine.MeterJsonIsLocaleIndependent` uses
  `GTEST_SKIP` when `de_DE.UTF-8` is unavailable (proot) — the single
  pre-existing skip in both builds, present since P4 (not a regression).