# SoundForge — Release Notes G6

**Gate:** G6 — Python Reference/Regression Parity (ctypes): the deferred G0 lane
"Python reference/regression parity — no NumPy engine yet" lands as an
end-to-end-testable host-side Python layer over the *real* C++ engine.
**Tag:** `g6-complete`. **Engine version:** `0.1.0-g6`. **Schema:** `project_v2`
(schemaVersion **2**, **unchanged** — G6 adds zero wire surface).

## Deliverables

- **Host shared library** `libsoundforge.so` — new `SF_BUILD_HOST_SHARED` option
  (default ON) gating a SHARED target `soundforge` in `native/src/host/`;
  links the five existing statics (`sfcore sfgraph sfdsp sfmeasure sfaudio`) +
  `Threads::Threads` + `stdc++fs` with `-Wl,--whole-archive` (mandated by
  ORC-G6-17: off → 0 `T sf_` exports, on → 65), pinned
  `LIBRARY_OUTPUT_DIRECTORY` → build root (ORC-G6-03), anchored by a generated
  1-line `host_stub.cpp` TU (ORC-G6-01). Guarded `NOT CMAKE_CROSSCOMPILING`
  (ORC-G6-18 — `ANDROID` is unusable as a guard under proot: it reports
  `ANDROID=1`/`SYSTEM_NAME=Android` with native gcc while `CROSS=FALSE`).
  **65 `T sf_` exports** (incl. the full §3.3 wrapped surface; all **11** audio
  engine exports); `sf_error_string` is NOT exported (header-inline, D1/D3).
  Built in **both** `native/build` (reg) and `native/build-asan` (UBSan).
- **ctypes loader + bindings** (`soundforge_py/engine.py`, D3) — stdlib-only
  `ctypes.CDLL`; **lazy** function-level load (importing never fails without a
  build; the 12 legacy tests stay green on source-only checkouts); opened with
  explicit **`RTLD_LOCAL`** (SEC-G6-02); path contract `$SOUNDFORGE_LIB_PATH`
  (must be **absolute**, `realpath`-canonicalized, `isfile`-checked — SEC-G6-01)
  → `REPO_ROOT/native/build/libsoundforge.so` → `RuntimeError` naming both
  candidate paths + a build hint. Security hardening (SEC-G6-01..08): absolute
  env path only; `RTLD_LOCAL` pinned; byte-length discipline via `_as_bytes`
  (SEC-G6-03); `_take_string()` sole native-memory free path (SEC-G6-04); io
  callback contract — copy-within-call, try/except → `SF_E_IO`, `channels==2`
  tripwire, `CFUNCTYPE` kept alive (SEC-G6-05); no `__del__`/finalizers,
  context-manager destruction only (SEC-G6-06); byte-identity feeding of
  fixtures (SEC-G6-07); conftest file-relative/system-isolated lazy fixture
  (SEC-G6-08).
- **Pure-Python reference engine** (`reference/engine.py`, D5) — chain-gain-only:
  topo order (rejects non-chain graphs with `ValueError`), per-node
  `10^(gainDb/20)`, block rendering in topo order, output = target node block,
  reference meter = max abs over the output block. DC-only meter parity (true
  peak of a constant == its amplitude, no FIR); sine confined to output-*block*
  parity (G6-1).
- **Regression/parity harness** (`regression/engine_parity.py`, D4) — signal
  generators + compare helpers; single source of tolerances (ORC-G6-10);
  consumed by the parity suite.
- **Parity suite** (`tests/python_tests/test_engine_parity.py`) — **34 parity
  cases** (P1: version/schema lockstep, validate decision parity ×5 fixtures,
  migrate normalized equality v0→2/v1→2, project canonical-form round-trip +
  getters + non-ASCII; P2: audio anchors, reset_meters monotonicity, sine block
  parity; P3: chain/block-size matrix 4×2, v0-migrate + save/open round-trip
  over `tmp_path`, channel agreement, 3 negative cases) + **12 legacy** schema
  tests = **46 total**, green against **both** reg and UBSan `.so` builds (D6).

## Core decisions (D1–D6, PLAN_G6 §3)

- **D1 — ctypes over cffi/pybind11.** The native surface is already a C ABI
  (every public header `extern "C"`, caller-freed memory via `sf_free_string`),
  so ctypes maps it 1:1 with zero glue compilation. pybind11 re-wraps a C-ABI
  ctypes already consumes and adds FetchContent/build-time coupling to Python
  dev headers; cffi ABI-mode is ctypes with extra install friction. Both add
  cost for zero coverage — rejected. Every binding carries explicit
  `argtypes`/`restype` (no default conversions); the parity suite doubles as an
  ABI smoke test (ORC-G6-13: explicit 65-symbol name-list gate, not a bare ≥39
  count).
- **D2 — host-only SHARED target** over the five unchanged statics; whole-archive
  mandatory (ORC-G6-17); default visibility; `NOT CMAKE_CROSSCOMPILING` guard
  (ORC-G6-18); no install target (build-tree resolution, D3); no change to
  `sf_unit_tests` link set or ctest counts.
- **D3 — loader path contract + wrapped surface** (~39 symbols: version 3, schema
  1, migration 1, project 17, string/error utils 3, command queue 2, audio 11)
  with the hardened path/callback/memory rules (SEC-G6-01..08, above).
- **D4 — parity harness: authority `.so`, reference pure-Python, harness
  shared.** Deterministic surface order: version/schema → validate decision →
  migrate **normalized** dict-equality (UUID/RFC3339 masked on both sides,
  auditLog as ordered tuples, `engineVersion` strict — raw equality is
  unimplementable: both migrators inject fresh UUIDs + wall-clock ms; ORC-G6-04)
  → project **canonical-form** round-trip idempotence (pass1==pass2==pass3, no
  raw-bytes compare; ORC-G6-06) → audio tick→meter/block parity. **Byte
  identity** (SEC-G6-07): the project document is serialized **once** as Python
  bytes and those exact bytes feed every native entry + `json.loads` on the
  reference — no re-dump, which could mask a UTF-8/escaping differential.
- **D5 — reference scope minimal-but-honest:** chain-gain only; everything richer
  (TruePeak FIR reimplementation, pan, mixer math, multi-source, power/acoustics)
  is a documented residual (G6-1) for the deferred NumPy-grade engine.
- **D6 — parity against BOTH build trees** (reg primary, UBSan required via
  `SOUNDFORGE_LIB_PATH`). UBSan dlopen is safe here: `build-asan` is UBSan-only
  (no ASan ordering requirement); TSan stays excluded (G6-7). `RTLD_LOCAL`
  (SEC-G6-02) means the .so's mangled C++ interior can never interpose on other
  DSOs. A genuine UBSan finding aborts loudly (`-fno-sanitize-recover=all`) →
  red parity run, never a silent miss.

## Anchors & tolerances (D4/D5, ORC-G6-10)

| Chain (`src`→`out`), output `out` | Block samples / meter `truePeakLinear` |
|---|---|
| `src(-6 dB)→out(0 dB)` | **0.501187** (`10^(-6/20)`) |
| `src(-6)→out(+6)` | **1.0** (`0.501187 × 1.995262`) |
| `src(+6)→out(0)` | **1.995262** with `clipped == true` |
| silence (`io` NULL) | `truePeakLinear == 0.0` **and** `truePeakDb == null` (both channels; ORC-G6-05 — NOT null linear) |

- Tolerance per surface (single source in `regression/engine_parity.py`):
  **meter** `|native − reference| ≤ 1e-3` (DC only, mirrors unit tests);
  **DC-anchor block samples ≤ 1e-6 absolute** (mirrors test_audio_engine.cpp);
  **sine block samples ≤ 1e-5 relative** (`1e-5 × (1 + |ref|)`; float64
  reference vs float32 engine).
- Meter-anchor is the **post-reset steady-state** value (primed cycle 8×tick →
  `reset_meters` → 1 tick, `blocksRendered == 9`); **block-anchor applies on
  every tick** from tick 1 (no priming); the cold-onset **FIR-latch overshoot
  (~×1.1259)** persists until reset (ORC-G6-P2-05 — the anchor contract is
  post-reset, so the overshoot is not part of it).

## Residuals

| # | Residual | Sev | Disposition |
|---|---|---|
| G6-1 | Reference engine is chain-gain-only (no pan/mixer/multi-source/TruePeak-FIR); **sine meter parity not covered** (DC-only meter parity) | LOW | Documented boundary in `reference/engine.py` + release notes; grows into the deferred NumPy engine without breaking the parity contract |
| G6-2 | ctypes wrapper covers ≈39 of ≈65 exports; diagnostics, geometry, graph mutators, `apply_batch`, standalone queue runner, enqueue/dequeue unwrapped | LOW | Explicit scope boundary; parity only wraps what parity exercises |
| G6-3 | No pip packaging/entry points/install target; pytest runs from the source tree | LOW | SEC-G6-09: re-review the dlopen posture (`SOUNDFORGE_LIB_PATH`, `RTLD_LOCAL`) **before** any packaging gate |
| G6-4 | `python/data_import/` remains a stub | LOW | Belongs to the native measurement-import lane |
| G6-5 | `app/src/main/cpp/CMakeLists.txt` keeps stale `0.1.0-g0` stamp | LOW | Android-static-review-only; **KEEP** by design (Q5) — no 8th surface |
| G6-6 | Python `migrate.py`/`schema.py` remain mirrors, not forks | LOW | Lockstep test-enforced; any spec change must land both sides in one commit |
| G6-7 | TSan build excluded from parity | LOW | TSan libs cannot be dlopened into an uninstrumented process; threading unit-covered |
| G6-8 | Parity callbacks bypass the header's lock-free/alloc-free advisory | LOW | Safe: `start(0)` (no pacer), callbacks on the calling Python thread (ORC-G6-11, SEC-G6-05); a future PACE gate must be GIL-safe |
| G6-9 | **`dspchain_v2` round-trip is lossy**: native codec drops `signalGraph.mixers` on `to_json` (json_codec.cpp:124–127,236–240) | LOW | Pre-existing native gap (ORC-G6-07); `dspchain_v2` excluded from the round-trip corpus; tracked for a future native gate |
| — | NumPy DSP engine | — | **Deferred** by design (G0 lane wording); reference is pure-Python chain-gain |
| — | mono≡stereo definition | — | Defined and tested (`test_mono_stereo_channel_agreement`): same gain chain rendered on stereo (both channels filled from the same source) → meter L/R agree (DC); no separate mono rendering path exists |
| — | Cold-onset overshoot (~×1.1259) not asserted | — | Native-side evidence gap: the FIR-latch overshoot is documented (ORC-G6-P2-05) but not asserted — the post-reset anchor contract is what parity pins |
| — | pan / solo / multi-source | — | **Out of scope** for D5: reference raises `ValueError` ("out of D5 chain-gain scope") instead of silently mis-rendering (ORC-G6-P2-04) |

## Commits (P0–P4)

| Commit | Phase |
|---|---|
| `f528556` | plan(g6): PLAN_G6.md — python reference/regression parity (ctypes) |
| `f4f094e` | docs(g6): gate amendments — ORC-G6-01..17 + SEC-G6-01..10 landed |
| `fa23481` | docs(g6): p1 escalation — host guard NOT CMAKE_CROSSCOMPILING (ORC-G6-18) |
| `099728c` | p1(g6): host libsoundforge.so + ctypes loader + parity subset (version/schema/migrate/project) |
| `f17f723` | p2(g6): audio engine ctypes surface + tick/meter/block parity vs pure-python reference |
| `3886ac2` | fix(g6): audio teardown hardening — close respects destroy result (ORC-G6-P2-01) |
| `0dd5854` | docs(g6): p2 gate — D4 meter-anchor wording + P2-01..05 record (ORC-G6-P2-05) |
| `7fc4bf3` | p3(g6): reference/regression packages + parity matrix expansion + python docs |
| `2f8642d` | fix(g6): save/open round-trip covers open_from_path + README counts (ORC-G6-P3-01/02) |
| (this) | p4(g6): version 0.1.0-g6 + release notes + DoD sweep |

## Versions

| Stamp | Where |
|---|---|
| `0.1.0-g0` | `app/src/main/cpp/CMakeLists.txt` — **KEEP** by design (G6-5, Q5); Android static-review-only |
| `0.1.0-g5` | pre-P4 status of the seven G6 surfaces (superseded) |
| `0.1.0-g6` | **all seven** §4.2 surfaces: `sf_version.h` (`-g6`), `native/CMakeLists.txt` (`SF_BUILD_VERSION`), `version.cpp` fallback, `version_gen.h.in` (comment), `python/__init__.py` (`__version__`), `migrate.py` (`ENGINE_VERSION`), `pyproject.toml` (+ "stubs" dropped from description) — one commit, both caches reconfigured |

## Verification (host, PLAN_G6 §6.4 + §7)

| Check | Result | Note |
|---|---|---|
| `ctest -R Version` (both trees) | **4/4, `0.1.0-g6`** | `sf_engine_version() == "0.1.0-g6"` on both reconfigured caches |
| ctest (both trees, incl. UBSan) | **314 registered / 313 passed, 1 skipped** | the single `MeterJsonIsLocaleIndependent` locale skip (de_DE.UTF-8 unavailable under proot) |
| pytest `tests/python_tests` (reg + UBSan override) | **46/46 both** | 12 legacy + 34 parity; includes the 3 migrated `0.1.0-g6` pins (lockstep, migrate-anchor, save/open pin) |
| `nm -D --defined-only` `T sf_` (both trees) | **65 / 65**; `sf_error_string` **0** | explicit §3.3 wrapped-name-list gate (ORC-G6-13) |
| drift diff canonical ↔ golden | **empty** | `project_schema.json` byte-identical to `schema_golden_v2.json`; pins untouched |
| JNI export grep | **26** | G6 adds no JNI; `app/**` untouched |
| `SF_E_*` codes | **7** | `sf_types.h` unchanged (no new error codes) |
| tree | **clean** | after the single P4 commit |

Android: static-only in this container (no SDK/NDK) — the documented G0–G3
limitation stands. The engine is host-only by design; the Android device lane
remains `NOT VERIFIED`.

## Environment limitations

- **ASan is proot-blocked**; **UBSan** (`native/build-asan`,
  `-fsanitize=undefined -fno-sanitize-recover=all`) is the sanctioned
  sanitizer substitute — full suite green. **TSan remains BLOCKED** (G3-8) and
  is excluded from parity (G6-7: TSan libs cannot be dlopened into an
  uninstrumented process).
- **Android is static-only** in this container — G2-4 stands.
- **Locale test skip:** `AudioEngine.MeterJsonIsLocaleIndependent` uses
  `GTEST_SKIP` when `de_DE.UTF-8` is unavailable (proot) — the single
  pre-existing skip in both builds (present since G4 P4; not a regression).

*End of RELEASE_NOTES_G6.md*