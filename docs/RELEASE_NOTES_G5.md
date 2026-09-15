# SoundForge — Release Notes G5

**Gate:** G5 — Live Engine Control + G4 Hardening: `SF_CMD_SET_OUTPUT` (cmd 8),
runner interception → plan recompile/publish without touching engine internals.
**Tag:** `g5-complete`. **Engine version:** `0.1.0-g5`. **Schema:** `project_v2`
(schemaVersion **2**, **unchanged** — G5 adds zero wire surface).

## Deliverables

G4 shipped a host-testable audio engine with a fixed output target (set at
`CREATED` state, compiled into snapshot #0 at `start()`). G5 closes the
**G4-4 residual** ("output target fixed pre-start") with a **live-control
command** (`SF_CMD_SET_OUTPUT`, cmd 8) that the runner intercepts on its own
thread, triggering a fresh plan compile and publish without the runner touching
the engine instance — the next rendered tick reads the new target. G5 also
implements **two hardening lanes** for the G4 residuals (G4-8, G4-9),
reclassifies one (G4-6 — premise false), and declines one (G4-5 — YAGNI):

- **Live-control command** (`SF_CMD_SET_OUTPUT`, cmd 8): runner intercepts,
  extracts target id from `cmd.id1` (bounded copy via `strnlen` —
  SEC-G5-01), fires the observer's internal `set_output` callback (opaque
  `user`, no engine-struct naming in core), engine-side callback updates
  `out_node_id` and re-publishes. Next tick renders the new target.

- **Project-destroy skip-free guard** (G4-8, P4): `sf_project_destroy` (void API)
  guards against attached CREATED engine via acquire-load on `audioEngine`;
  guard skips the free + sets handle error + returns (mirrors SEC-G3-2); caller
  must `sf_audio_engine_destroy` first, then destroy project again. Testable via
  `sf_last_error` + retry-destroy-succeeds; zero ABI change.

- **Allocator alignment overloads** (G4-9, P1): `operator new/delete[/]/[]` with
  `std::align_val_t` added to `alloc_counter` test fixture; closes the
  zero-alloc-assertion evasion hole; all consumers use delta assertions so no
  existing test flips.

- **Meter serialization documented** (G4-6, P3): Shipped code contains **no**
  `std::atomic<double>` — meter is plain `double m_latch[2]` (true_peak.hpp:71)
  guarded by ONE engine `std::mutex meter_mu` (audio_engine.cpp:98–101,
  SEC-G4-02(a)); no generation guard exists or is needed. P3 documents this real
  mechanism; the G4-6 portability concern is **defunct** (premise false).

- **Plan memory (G4-5, declined at gate)**: OOM is contained by design and
  *tested* through the `CompileFn` fault-injection seam (snapshot.hpp:78–79);
  publish failure restores the slot to EMPTY and the **previous READY plan stays
  current** (snapshot.hpp:16–18), so a cap would refuse legal large docs for a
  hypothetical problem — YAGNI. No code, document-only.

## Gate Record

### P0 — Plan commit
- **Verdict:** AMEND → `docs(g5): gate amendments` (ses_f5a14240effeEdziVR8PFhO13w, 2026-09-15).
- **Oracle amendments (ORC-G5-01..07):**
  - F1: P5 amplitude calculation corrected (fixture now src(-6dB)→out(+6dB) with observable amplitude delta).
  - F2: Two-session test posture established (runner is one-shot; separate engine instances).
  - F3: G4-6 premise false; meter is mutex-serialized, not atomic<double>.
  - F4: `sf_project_destroy` is void; skip-free guard instead of error return.
  - F5: Store semantics: compile failure → `planValid:false`, unknown target → valid silence.
  - F6: Version surfaces verified complete (CMakeLists.txt, sf_version.h, version.cpp, __init__.py, migrate.py).
  - F7: `test_cmd_queue.cpp` optional; accepted.
- **Security amendments (SEC-G5-01..03):**
  - Unterminated `id1` (64 bytes, no NUL): interception bounds via `strnlen(cmd.id1, sizeof(cmd.id1))`.
  - Unknown target → valid plan rendering silence (not a compile failure).
  - Pre-G5 runner binaries drop unknown command types with ERROR log (logged, not silent).

### P1 — `SF_CMD_SET_OUTPUT` command interception + allocator alignment
- **Commits:**
  - `61d71ca` — SF_CMD_SET_OUTPUT (cmd 8): runner interception + engine callback + allocator aligned overloads.
- **Acceptance:**
  - Command type 8 constant defined, round-trips queue.
  - Runner intercepts before `apply_batch_impl`, fires observer callback.
  - SEC-G5-01: unterminated id1 negative test (64 bytes, no NUL, drained safely, no overread).
  - JNI stays 26; allocator overloads instrumented; all G3/G4 tests pass.

### P2 — Memory hardening (DECLINED at gate)
- **No commit:** OOM containment rationale documented in release notes (G5-1 residual).

### P3 — Meter serialization documentation
- **Commit:**
  - `f18c8b1` — meter mutex-serialization documented (G4-6 reclassified — no atomics in codebase).
- **Acceptance:**
  - Comment in `sf_audio_engine.h` documents real mechanism: one mutex serializes meter reads/writes.
  - No code change; G4-6 residual corrected.

### P4 — Project-destroy skip-free guard
- **Commit:**
  - `b5130f7` — project-destroy skip-free guard (G4-8) — mirrors SEC-G3-2.
- **Acceptance:**
  - Guard at top of `sf_project_destroy`: acquire-load on `audioEngine`; if attached, log ERROR + set handle error + return.
  - Test (a): engine destroyed first → project destroy succeeds (no guard). (b): skip destroy → guard skips free, retry succeeds.
  - Zero ABI change.

### P5 — Live-control e2e + version + release notes (this commit)
- **Commits:**
  - (this) — p5(g5): live output retarget e2e + version 0.1.0-g5 + release notes.
- **Acceptance:**
  - **Fixture:** `set_chain_gains(p, src_db, out_db)` extends `set_chain` with per-node gains.
  - **Session A (same-target retarget):** chain `src(-6 dB) → out(+6 dB)`, target "out" ≈1.0; retarget "out" → "out"; next tick ≈1.0 (benign).
  - **Session B (observable retarget):** chain `src(-6 dB) → out(+6 dB)`, target "out" ≈1.0; retarget to "src"; next ticks render "src" ≈0.501187 (observable), latch updates accordingly after meter reset.
  - No `project.migrate` audit entry; schemaVersion stays 2.
  - Amplitude assertions @1e-6 (both per F1).
  - Full §7 suite green: ctest reg (313 passed + 1 locale skip) + UBSan (313 passed + 1 skip), pytest 12/12, drift empty, grep 26, Version 4/4, SF_E_* 7.

## Engine ABI (host, `sf_audio_engine.h`)

11 host-only exports: unchanged from G4. Lifecycle `{CREATED, STARTING, RUNNING, STOPPING, STOPPED}`; one-shot start; destroy requires not-RUNNING/not-STOPPING (auto-joins pacer then runner). `tick` is deterministic (no wall clock); IO struct is **host protocol** — never wired to JNI.

## Snapshot protocol (as shipped)

`PlanSnapshotStore`, 4 slots, per-slot state `{EMPTY, WRITING, READY, READING}` + monotone `seq`; single writer (runner thread, after each applied batch **or live retarget**) / single reader (tick/pacer thread); publish wait-free (4th-slot guarantee); acquire = largest-`seq` READY → CAS READING with post-CAS authoritative `seq` re-read.

## Meter serialization (as shipped — G4-6 corrected)

Meter fields are plain `double` guarded by a single engine `std::mutex meter_mu` that serializes `TruePeak::process/latch/reset` and `meter_json/reset_meters` (SEC-G4-02(a)). No per-field atomics, no generation guard: the mutex prevents torn reads. Render kernels themselves are lock-free.

## Verification (host, PLAN_G5 §6.5)

| Check | Result | Note |
|---|---|---|
| `cmake --build native/build && ctest` | **314 registered / 313 passed, 1 skipped** | 313 + 1 locale skip (de_DE.UTF-8); 6 net-new across P1/P4/P5 (G4 end 308 → 314, zero removals): 2 cmd-8, 2 destroy-order, 2 live-control |
| UBSan build `ctest` (`native/build-asan`) | **314 registered / 313 passed, 1 skipped** | `-fsanitize=undefined -fno-sanitize-recover=all`; same 1 locale skip |
| `ctest -R Version` | **4/4** | `sf_engine_version() == 0.1.0-g5`; `SF_ENGINE_VERSION_SUFFIX` `-g5` |
| `pytest tests/python_tests -q` | **12 passed** | unchanged (version string only) |
| drift diff canonical ↔ golden | **empty** | `project_schema.json` byte-identical to `schema_golden_v2.json` |
| JNI export grep | **26** | `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` — G5 adds **no** JNI |
| `SF_BUILD_VERSION` in both caches | **`0.1.0-g5`** | `native/build` + `native/build-asan` reconfigured |
| `sf_audio_engine_s` in core | **0 refs** | `grep -rn "sf_audio_engine_s" native/src/core/` — opaque-user discipline preserved |
| schemaVersion | **2** | `SF_SCHEMA_VERSION` untouched; no migration; no `project.migrate` |
| New `SF_E_*` codes | **7** | `sf_types.h` unchanged (no new error codes) |

Android: static-only in this container (no SDK/NDK) — the documented G0–G3 limitation stands. The engine is host-only by design; the Android device lane remains `NOT VERIFIED`.

## Commits (P0–P5)

| Commit | Phase |
|---|---|
| `2c253c6` | plan(g5): PLAN_G5.md — live engine control + G4 hardening lanes |
| `a256d21` | docs(g5): gate amendments — ORC-G5-01..07 + SEC-G5-01..03 landed |
| `61d71ca` | p1(g5): SF_CMD_SET_OUTPUT (cmd 8) — live output retarget interception |
| `f18c8b1` | p3(g5): meter mutex-serialization documented (G4-6 reclassified — no atomics in codebase) |
| `b5130f7` | p4(g5): project-destroy skip-free guard (G4-8) — mirrors SEC-G3-2 |
| (this) | p5(g5): live output retarget e2e + version 0.1.0-g5 + release notes |

## Residuals

### G4 residuals — disposition in G5

| # | G4 residual | Disposition |
|---|---|---|
| G4-1 | Real Android device | **STANDS** — G5 adds no Android |
| G4-2 | True peak not BS.1770-certified | **STANDS** |
| G4-3 | Pacer timing best-effort | **STANDS** |
| G4-4 | Output target fixed pre-start | **CLOSED** — G5 lands live `SF_CMD_SET_OUTPUT` command |
| G4-5 | Plan memory ~4 KB/node | **STANDS (declined, documented)** — OOM contained by design + `CompileFn` seam-tested |
| G4-6 | `std::atomic<double>` not portable | **SUPERSEDED** — premise false; meter is mutex-serialized (documented) |
| G4-7 | Independent two-reviewer gate | **RESOLVED** (G4 time; G5 inherits) |
| G4-8 | Project destroy cannot see engine | **CLOSED** — P4 lands skip-free guard |
| G4-9 | Allocator alignment | **CLOSED** — P1 lands aligned overloads |
| G4-10 | Callback error paths | **STANDS** (unchanged) |

### New G5 residuals (post-gate)

| # | Residual | Sev | Disposition |
|---|---|---|---|
| G5-1 | G4-5 (plan memory) deliberately not hardened | LOW | Memory scales with node count; OOM is contained (last-valid plan retained; never-published store renders silence). |
| G5-2 | Live retarget is best-effort timing (next-tick semantics, no latency SLA) | LOW | Contract: the next rendered tick after the command drains reads the new target. No determinism/SLA added (same posture as G4-3 pacer). |
| G5-3 | `engine_set_output` re-publish on unknown target → valid plan rendering silence | LOW | Unknown target compiles fine with `out_index==-1`, publish returns true, `planValid:true` (SEC-G5-02). `plan_valid=false` only on genuine compile failure (cycle/OOM), where the last-valid plan is retained. Same convention as static `set_output` (ORC-G4-06). |

## Definition of Done (PLAN_G5 §7)

| # | Item | Result |
|---|---|---|
| 1 | ctest green on `native/build` **and** `native/build-asan` (UBSan); pytest green; JNI export grep **26** | **PASS** — 313 + 1 skip / 313 + 1 skip; pytest 12/12; grep 26 |
| 2 | No drift: `project_schema.json` ≡ `schema_golden_v2.json`; pins untouched; v1 untouched; schemaVersion **2**; no migration, no new key, no new `SF_E_*` | **PASS** — drift empty; schemaVersion 2; no `project.migrate`; SF_E_* 7 (unchanged) |
| 3 | Live control: `SF_CMD_SET_OUTPUT` (cmd 8) enqueued, runner intercepts, updates engine's target, triggers plan recompile/publish; next tick renders new target | **PASS** — two e2e tests (same-target benign + observable retarget) |
| 4 | No new JNI / no Android: `app/**` untouched; export grep 26; no Kotlin mirror | **PASS** — `app/**` byte-identical to G4 baseline |
| 5 | Version stamp `0.1.0-g5` in `sf_version.h`, CMake default, both reconfigured caches, python (`__version__`, `ENGINE_VERSION`) | **PASS** — all surfaces stamped; `-R Version` 4/4 |
| 6 | Hardening decisions executed: G4-5 (decline documented), G4-6 (documented/stands), G4-8 (guard implemented), G4-9 (overloads added) | **PASS** — all four lanes handled per gate verdict |
| 7 | Docs: plan + `RELEASE_NOTES_G5.md`; tag `g5-complete` | **PASS (docs)** — this document; tag applied by the orchestrator after independent final audit |

## Environment limitations

- **ASan is proot-blocked**; **UBSan** (`native/build-asan`, `-fsanitize=undefined -fno-sanitize-recover=all`) is the sanctioned sanitizer substitute — full suite green. **TSan remains BLOCKED** (G3-8: `unexpected memory mapping` under proot). The new live-control surfaces (retarget command interception, observer callback, plan recompile on runner thread) are instead bounded by the explicit single-writer/single-reader protocol + drain-to-STOPPED barrier + lifecycle tests, all green under UBSan.
- **Android is static-only** in this container (no SDK/NDK) — G2-4 stands. The engine is host-only by design; the Android device lane remains `NOT VERIFIED`.
- **Locale test skips:** `AudioEngine.MeterJsonIsLocaleIndependent` uses `GTEST_SKIP` when `de_DE.UTF-8` is unavailable (proot) — the single pre-existing skip in both builds, present since P4 (not a regression).

*End of RELEASE_NOTES_G5.md*
