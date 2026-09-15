# SoundForge — Gate G6 Plan: Python Reference/Regression Parity —
# ctypes Layer over a Host Shared Library + Pure-Python Reference + Lockstep Suite

> **Status: DRAFT — scratch document for orchestrator review (NOT committed).**
> This document is the phase-0 plan contract for Gate G6. Reviewed and amendments
> applied per the gate process (final two-reviewer gate: oracle +
> security-reviewer, out-of-band) before P1 may begin. The plan specifies the
> deferred G0 lane **"Python reference/regression parity"** — an
> end-to-end-testable Python layer against the real C++ engine, HOST-side only.
> **Supersedes:** `docs/PLAN_G0.md`…`PLAN_G5.md` for G6 scope only. G0–G5 remain
> the contract for everything not changed here. G5 tag: `g5-complete`
> (`07a2e0a`).
> **Deliverable:** a `libsoundforge.so` host shared library (new SHARED target
> built from the existing five static libs), a stdlib-only **ctypes** binding
> (`soundforge_py/engine.py`), a **pure-Python reference engine**
> (`reference/engine.py`, chain-gain surfaces only — no NumPy), and a
> **lockstep parity pytest suite** (`tests/python_tests/test_engine_parity.py`)
> that compares the C++ engine and the Python mirror on deterministic surfaces:
> version/schema compatibility, project JSON round-trip, validate/migrate
> lockstep, and audio tick → meter/block parity. `app/**` stays KEEP; JNI stays
> 26; schemaVersion stays 2; no new `SF_E_*`.
>
> All recon facts below were re-verified against the tree on 2026-09-15 by the
> planner (git log `07a2e0a` HEAD clean; ctest reg + UBSan 314/314; pytest
> 12/12; JNI grep 26; `build-asan` is UBSan, not ASan — CMakeCache
> `CMAKE_CXX_FLAGS=-fsanitize=undefined -fno-sanitize-recover=all`).

---

## 1. G6 Scope & Non-Goals

### 1.1 Goal

The G0 deferred lane says: *"Python reference/regression parity — `python/*`
stubs only (no NumPy engine yet)."* Five gates later the situation is: the
Python package mirrors the *schema* and *migration* surfaces for real
(`migrate.py`, `schema.py` — validated by 12 pytest cases), but there is **no
way to execute the actual C++ engine from Python**: the native build is
static-only (`libsfcore.a` + four siblings, no `.so`), there is zero binding
machinery (no pybind11/cffi/ctypes/Python.h anywhere), and Android's only
shared lib is the app's own `libsfcore.so` (a different, JNI-wired source set
— wrong for host use).

G6 closes that gap with an end-to-end-testable **host-side** Python layer:

1. A **host-only SHARED library** `libsoundforge.so` built by a new target in
   the existing native CMake tree (links `sfcore + sfgraph + sfdsp + sfmeasure
   + sfaudio`; default symbol visibility; **not** built for Android).
2. A **stdlib-only ctypes binding** (`soundforge_py/engine.py`) wrapping
   ≈39 exported C-ABI symbols. No pip deps, no build step, no Python-dev
   headers, no numpy.
3. A **pure-Python reference engine** (`reference/engine.py`) that computes the
   *same deterministic surfaces* in Python (chain-gain block rendering; DC
   true-peak meter) — honest, tiny, exact — explicitly **not** a NumPy DSP
   engine.
4. A **lockstep parity pytest suite** (`tests/python_tests/test_engine_parity.py`)
   that runs the C++ engine via ctypes and the Python reference side by side
   and compares outputs on deterministic surfaces, in **both** the reg and the
   UBSan build trees.

### 1.2 G6 MUST deliver

| Pillar | Scope | Deliverable |
|---|---|---|
| **Host shared library** | `native/CMakeLists.txt` (EDIT), `native/src/host/CMakeLists.txt` (NEW) | New `SF_BUILD_HOST_SHARED` option (default ON on non-Android) gating a SHARED target `soundforge` → `libsoundforge.so`, linking the five existing statics + `Threads::Threads` + `stdc++fs`. Default visibility so all `extern "C"` exports are present. Built in **both** `native/build` and `native/build-asan` (UBSan) trees. Does not touch the static unit-test target, does not touch `app/src/main/cpp/CMakeLists.txt` (which never `add_subdirectory`s the native tree — verified). |
| **ctypes loader + bindings** | `python/soundforge_py/engine.py` (NEW P1, EDIT P2) | Stdlib `ctypes.CDLL` loader with a documented path contract (env `SOUNDFORGE_LIB_PATH` → `native/build/libsoundforge.so`), lazy load so `import soundforge_py.engine` never fails without a build, and bindings for ~39 symbols: version (3), schema (1), migration (1), project (17), string/error utils (3), command queue (2), audio engine (11). High-level Pythonic helpers + `AudioEngine`/`Project` context managers. |
| **Path (loader) contract** | `tests/python_tests/conftest.py` (NEW) | Resolution order: `$SOUNDFORGE_LIB_PATH` (absolute .so path; used to target the UBSan build) → `REPO_ROOT/native/build/libsoundforge.so` → clear `RuntimeError` naming both paths + `cmake --build native/build` hint. Conftest also provides `repo_root`, `fixtures_dir`, relative fixture paths — without touching `test_schema_py.py` (KEEP, golden pins untouched). |
| **Version/schema parity** | `test_engine_parity.py` (NEW P1) | `sf_engine_version()` == `__version__` == `0.1.0-gN`; `sf_schema_version()` == `SCHEMA_VERSION` == 2; `sf_is_compatible(0..2)==1`, `(3,-1)==0` — these make version drift a **test failure** (self-checking lockstep). |
| **Validate lockstep parity** | `test_engine_parity.py` (NEW P1) | Same shared fixture corpus (`tests/fixtures/`) through EITHER native `sf_validate_project_json` (structural mirror in `schema.cpp`) or Python `validate_project` (jsonschema on the canonical file): **accept/reject decision + error presence equal** per fixture. Messages are not compared (two different validators by design). |
| **Migrate lockstep parity** | `test_engine_parity.py` (NEW P1) | Same fixture through native `sf_migrate_json` (in-place, sized buffer) and Python `migrate_json`: **resulting documents dict-equal** (including `engineVersion` — both sides stamp the same gate version, so equality is the contract; the P4 bump moves both together). |
| **Project JSON round-trip parity** | `test_engine_parity.py` (NEW P1) | `sf_project_from_json` → `sf_project_to_json` (freed via `sf_free_string`) semantic-equality on v2 fixtures; name/schema-version getters lockstep. |
| **Audio engine ctypes surface** | `engine.py` (EDIT P2) | Create/configure/set_output/start/tick/stop/join/destroy/`meter_json`/`reset_meters` (+ `sf_cmd_queue_create/destroy`; the engine binds a caller-owned queue). `ctypes.CFUNCTYPE` io callbacks: a DC/sine source `read` and a block-capturing `write`. Lifecycle per test = one fresh engine session (runner is one-shot — F-doctrine). |
| **Pure-Python reference engine** | `python/reference/engine.py` (NEW P2) | Computes: chain topo order, per-node `mixer.gainDb` (dB→linear `10^(db/20)`, default 0.0) applied in that order, input block supplied by the *same* Python signal source the read callback uses, output block = target node block, reference meter = max abs over the output block. DC-only meter parity (true peak of DC == amplitude exactly, no FIR needed); sine used for output-block parity only. Explicitly chain-only (no pan/mixer/multi-source/TruePeak FIR). |
| **Parity harness helpers** | `python/regression/engine_parity.py` (NEW P2) | Single source of tolerances + compare helpers (`compare_meter`, `compare_blocks`, signal generators) consumed by the pytest cases: meter `\|Δ\| ≤ 1e-3` (mirrors unit tests), block samples ≤ 1e-5 relative. |
| **Parity suite expansion** | `test_engine_parity.py` (EDIT P3) | Parametrized chain matrix (anchor gains `(0,0)→1.0`, `(-6,0)→0.501187`, `(-6,+6)→1.0`, `(+6,0)→1.995262 w/ clip`), v0→v2 migrated-chain round trip via `save/open_from_path` over `tmp_path`, mono→stereo, silence path (null meters). |
| **Docs + packaging sync** | `python/README.md`, `python/pyproject.toml`, `docs/RELEASE_NOTES_G6.md` (P3/P4) | README rewritten for the reference/regression layer + run commands; pyproject version fixed from stale `0.1.0-g0`; release notes for the gate. |

### 1.3 G6 MUST NOT deliver (defer, leave stubs/unchanged)

- **NumPy DSP engine** — explicitly deferred (G0 lane wording). The reference
  engine is pure-Python, chain-gain-only, exact-surface parity. No new python
  dependency is added (stdlib + existing `jsonschema` only).
- **New `SF_E_*` codes** — the 7 codes stay. ctypes surfaces errors via
  `sf_last_error()`/`sf_last_error_global()` (`sf_error_string` is
  header-inline and **not** exported — documented in the wrapper).
- **`float[]` in the public ABI** — G6 adds **no** ABI surface at all; it only
  loads existing headers. The `float* const*` io protocol stays host-only and
  never crosses JNI.
- **Schema / wire change** — schemaVersion stays **2**; `project_schema.json`
  and both golden files + SHA-256 pins untouched; drift diff must stay empty.
- **Android / JNI** — `app/**` KEEP; the host `.so` is guarded out of Android
  toolchains; JNI export grep stays **26**. Android remains static-review-only.
  `app/src/main/cpp/CMakeLists.txt` keeps its stale `0.1.0-g0` stamp
  (untouched by design — see residual G6-5).
- **Native TU edits** — G6 touches **no** `.cpp`/`.hpp` under `native/src/**`
  except `version.cpp` (P4 fallback string) and `version_gen.h.in` (P4 comment);
  headers, graph, dsp, audio, measurement sources are KEEP. The shared lib must
  link the existing statics **unchanged** (a pure packaging/link gate for the
  native layer). If a linker error demands a TU edit, that is a P1 gate
  escalation (orchestrator + oracle), not a silent amendment.
- **Full ctypes coverage of every export** — ≈60 `sf_` symbols exist; G6 wraps
  ≈39 (parity-exercised surfaces) and explicitly leaves diagnostics, geometry,
  graph mutators, `sf_graph_apply_batch`, standalone queue-runner, and
  enqueue/dequeue unwrapped (residual G6-2).
- **No fixed sleeps in tests** — parity tests are fully deterministic
  (tick-based, no pacer in parity paths; the engine documents `tick` as
  deterministic with no wall clock). No `time.sleep`, no busy-wait barriers in
  Python.
- **pip packaging / entry points** — no console scripts, no `pyproject`
  build for the tests; pytest runs from the source tree exactly as today
  (residual G6-3).

### 1.4 Gating rule

Same spirit as G0–G5 with a Python-specific adaptation: new native file is
**build config only** (`src/host/CMakeLists.txt`, no C++); new Python files
carry a LOC budget per module (declaration-table style over per-function
boilerplate): `engine.py` ≤ ~320, `reference/engine.py` ≤ ~150,
`regression/engine_parity.py` ≤ ~120, `conftest.py` ≤ ~60,
`test_engine_parity.py` ≤ ~280. Existing files receive small, focused
amendments only (version strings in P4; README/pyproject in P3/P4). The native
`≤100 LOC/new-file` rule does not apply to Python, but the bounded-contract
doctrine does.

---

## 2. File / Module Breakdown

Canonical root: `/root/project/soundforge/`. Tags: **NEW** / **EDIT** / **KEEP**.

```
soundforge/
├── native/
│   ├── CMakeLists.txt                      # EDIT (P1) — SF_BUILD_HOST_SHARED + add_subdirectory(src/host)
│   ├── src/host/
│   │   └── CMakeLists.txt                  # NEW (P1) — SHARED soundforge → libsoundforge.so
│   ├── include/soundforge/
│   │   ├── sf_version.h                    # EDIT (P4) — SUFFIX "-g6" (comment history line)
│   │   └── (all other headers)             # KEEP
│   └── src/{core,graph,dsp,audio,measurement}/
│       └── version.cpp                     # EDIT (P4) — fallback "0.1.0-g6"
│       └── version_gen.h.in                # EDIT (P4) — comment-only bump
│       └── (all other TUs)                 # KEEP
├── python/
│   ├── pyproject.toml                      # EDIT (P4) — version 0.1.0-g6 (+ description refresh)
│   ├── README.md                           # EDIT (P3) — reference/regression layer + run commands
│   ├── soundforge_py/
│   │   ├── __init__.py                     # EDIT (P4) — __version__ "0.1.0-g6"
│   │   ├── migrate.py                      # EDIT (P4) — ENGINE_VERSION "0.1.0-g6" (logic untouched)
│   │   ├── schema.py                       # KEEP
│   │   └── engine.py                       # NEW (P1) / EDIT (P2) — ctypes loader + bindings
│   ├── reference/
│   │   ├── __init__.py                     # EDIT (P3) — package docstring
│   │   └── engine.py                       # NEW (P2) / EDIT (P3) — pure-Python chain renderer
│   └── regression/
│       ├── __init__.py                     # EDIT (P3) — package docstring
│       └── engine_parity.py                # NEW (P2) / EDIT (P3) — tolerances + compare helpers
├── tests/
│   └── python_tests/
│       ├── conftest.py                     # NEW (P1) — libsoundforge resolution + shared fixtures
│       ├── test_schema_py.py               # KEEP (12 tests, golden pins untouched)
│       └── test_engine_parity.py           # NEW (P1) / EDIT (P2, P3) — lockstep parity suite
└── docs/
    ├── PLAN_G6.md                          # this document (P0)
    └── RELEASE_NOTES_G6.md                 # NEW (P4) — gate artifact
```

**KEEP (explicitly untouched):** `sf_types.h` (7 codes), all public header
signatures, `project_schema.json`, `tests/golden/*` + both SHA-256 pins,
`tests/fixtures/*`, `tests/unit/*` (no native test edits in G6),
`tests/python_tests/test_schema_py.py`, `python/soundforge_py/schema.py`,
`migrate.py` logic, `python/data_import/` (stub stays), `app/**`, docs G0–G5.

> **Bound check:** if a path is not listed above, do not create or edit it in G6.
> The native layer is consumed **unchanged**; G6 adds only a link/packaging
> target and Python-side files.

---

## 3. Architecture Decisions

Each decision: **Decision** / **Rationale** / **Acceptance**.

### 3.1 D1 — Binding technology: **ctypes (stdlib)**

**Decision.** Use Python's stdlib **`ctypes`** (`ctypes.CDLL` + `CFUNCTYPE`
for the io callbacks) as the sole binding mechanism. No cffi, no pybind11, no
`Python.h`, no new pip dependency, no build step.

**Rationale.**
- The native surface is **already a C ABI**: every public header is
  `extern "C"`-wrapped, all types are C-struct/primitives/opaque handles, and
  all memory the caller must free is freed by `sf_free_string`. ctypes maps
  this 1:1 (`int32_t→c_int32`, `size_t→c_size_t`, `char*→c_char_p`,
  opaque handles→`c_void_p`) with zero glue compilation.
- **pybind11** would re-wrap a C-ABI that ctypes already consumes — it adds a
  FetchContent/pip fetch, a native extension build step (build-time coupling to
  Python dev headers, breaking the host-only, no-toolchain posture), and
  nothing the C-ABI doesn't already expose. Rejected as cost without benefit.
- **cffi** needs a pip dependency and an ABI-mode or build-mode step; ABI-mode
  cffi is ctypes with extra install friction; build-mode needs a Python-aware
  build. The G6 constraint "no new deps, no SDK/NDK, no build-time coupling"
  selects ctypes outright.
- The only genuinely tricky ctypes surface — function-pointer callbacks
  receiving `float* const*` (`sf_audio_engine_io_t`) — is exactly what
  `CFUNCTYPE` + per-channel `array('f')` buffers handle cleanly, and it is
  exercised only in P2 parity tests with blocks ≤ 512 frames.
- Risk mitigated by design: manual signature declarations can drift from the
  headers. Mitigation: every binding is declared from one table annotated with
  the header symbol; the parity suite doubles as an ABI smoke test (a wrong
  signature shows up as a non-`SF_OK` result, garbage, or a parity mismatch in
  a deterministic compare — never silent).

**Acceptance.**
- `import soundforge_py.engine` succeeds with **zero** non-stdlib imports
  (jsonschema stays only in `schema.py`, untouched).
- All ~39 wrapped symbols carry explicit `argtypes`/`restype`; no `ctypes`
  default-conversion path is used.
- `nm -D --defined-only native/build/libsoundforge.so | grep ' T sf_'` shows
  ≥ 39 symbols including `sf_engine_version`, `sf_project_create`,
  `sf_audio_engine_create`, `sf_migrate_json`, `sf_validate_project_json`,
  `sf_free_string`, `sf_last_error*`, and **does not** show `sf_error_string`
  (header-inline, not exported — wrapper uses `sf_last_error()`).

### 3.2 D2 — Shared-library artifact: new host-only SHARED target `soundforge`

**Decision.** Add to `native/CMakeLists.txt`:

```cmake
option(SF_BUILD_HOST_SHARED "Build host shared libsoundforge.so (Python ctypes layer)" ON)
if(SF_BUILD_HOST_SHARED AND NOT ANDROID)
  add_subdirectory(src/host)
endif()
```

and create `native/src/host/CMakeLists.txt`:

```cmake
# G6 — host-only shared library for the Python ctypes parity layer.
# Links the five existing STATIC libs unchanged; default visibility exports
# the extern "C" surface. Android NEVER builds this target (guarded by
# SF_BUILD_HOST_SHARED AND NOT ANDROID at the top level).
add_library(soundforge SHARED)
target_link_libraries(soundforge PUBLIC
    -Wl,--whole-archive                                    # require ALL objects from the
    sfcore sfgraph sfdsp sfmeasure sfaudio                 # five statics (below)
    -Wl,--no-whole-archive
    Threads::Threads stdc++fs)
set_target_properties(soundforge PROPERTIES
    OUTPUT_NAME soundforge
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET default   # explicit: keep default visibility
    VISIBILITY_INLINES_HIDDEN OFF)
```

> **ORC-G6 pre-gate amendment (D2, orchestrator review):** `-Wl,--whole-archive`
> is **mandatory**. A shared library with no source files has no undefined
> symbols of its own, and GNU/LLVM ld extract from a static archive **only the
> object files that resolve an undefined symbol** — so without whole-archive
> semantics `libsoundforge.so` would link but export **nothing**. The
> implementer may use the modern `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>` generator
> expression (CMake ≥ 3.24) or the plain `-Wl,--whole-archive … --no-whole-archive`
> form above; the D1 `nm -D` gate (≥39 `sf_` symbols) is the acceptance check
> either way.

**Rationale.**
- **Where it lives.** `src/host/` mirrors the existing per-module layout; the
  top-level guard (`AND NOT ANDROID`) makes the "host-only" property explicit
  and future-proof even if an Android build ever `add_subdirectory`s the native
  tree (it does not today — `app/src/main/cpp/CMakeLists.txt` recompiles its
  own source list and documents that it deliberately does NOT
  `add_subdirectory`).
- **Not disturbing anything.** `sf_unit_tests` keeps linking the statics as
  before (no target-name clash: the test binary links `sfcore` etc.; the new
  target is `soundforge`). The statics are already built with
  `CMAKE_POSITION_INDEPENDENT_CODE ON` (top-level), so linking them into a
  shared object is clean. `sfcore → sfgraph → sfdsp` PRIVATE edges are
  satisfied by listing all five statics explicitly; CMake dedupes the repeated
  static in the transitive closure.
- **No install target.** Python resolves the artifact from the build tree (D3),
  mirroring every existing test convention (paths resolved relative to test
  files; no packaging steps). An install/package step is deferred (G6-3).
- **`stdc++fs`** mirrors `sf_unit_tests` (portable `std::filesystem` linkage
  across GCC versions) — harmless on modern compilers.
- **Visibility.** The top-level project never sets `-fvisibility=hidden`, and
  `CXX_VISIBILITY_PRESET default` pins it against toolchain defaults that might.
  All exports are the existing default-visibility `extern "C"` symbols — zero
  `__attribute__((visibility))` edits to headers.

**Acceptance.**
- `cmake --build native/build` and `cmake --build native/build-asan` both
  produce `libsoundforge.so` (P1 acceptance lists `ls` of both trees).
- `sf_unit_tests` target/link set is byte-unchanged (no CMake edit touches
  `tests/unit/CMakeLists.txt`); ctest counts stay 314/314 on both trees.
- Android app CMake untouched; JNI grep stays 26.
- `nm -D` shows the C-ABI symbols exported with **default** visibility
  (D1 acceptance).

### 3.3 D3 — Loader path contract and wrapped surface

**Decision.** `soundforge_py/engine.py` resolves the library in this order:

1. `$SOUNDFORGE_LIB_PATH` — absolute path; the *named* mechanism for pointing
   pytest at `native/build-asan/libsoundforge.so` (D6).
2. `REPO_ROOT / "native" / "build" / "libsoundforge.so"` — default (reg build).
3. Otherwise `RuntimeError` naming both tried paths and a
   `cmake --build native/build` hint.

Loading is **lazy** (function-level, cached in a module singleton): importing
`soundforge_py.engine` never fails without a build, so the 12 legacy tests and
any non-parity import path stay green on a source-only checkout. Wrapped
surface (≈39 symbols, header → functions):

| Header | Wrapped | Notes |
|---|---|---|
| `sf_version.h` | `sf_engine_version`, `sf_schema_version`, `sf_is_compatible` | lockstep vs `__version__`/`SCHEMA_VERSION` |
| `sf_schema.h` | `sf_validate_project_json` | sized err buffer (4 KB); decision-level result |
| `sf_migration.h` | `sf_migrate_json` | in-place `create_string_buffer`; `inout_len`/`cap` semantics |
| `sf_project.h` | create/destroy/clone/to_json/from_json/save_to_path/open_from_path + 6 getters + rename/venue/scene mutators + health_check (17) | `char** out_json` via `POINTER(c_char_p)` + `sf_free_string`; getters via `c_char_p.value` |
| utils | `sf_free_string`, `sf_last_error`, `sf_last_error_global` | error text path: `sf_error_string` NOT exported (header-inline) |
| `sf_command_queue.h` | `sf_cmd_queue_create`, `sf_cmd_queue_destroy` | engine binds a caller-owned queue (must outlive engine) |
| `sf_audio_engine.h` | all 11 exports | `AudioEngine` context manager; `CFUNCTYPE` io callbacks |

Explicitly **not** wrapped in G6: diagnostics (3), geometry (4), graph
mutators (9), `sf_graph_apply_batch`, standalone queue runner (6),
`enqueue/dequeue/depth` (G6-2).

**Rationale.** The loader contract is a **test-facing API** (like
`FIXTURES_DIR` for the unit tests), and the build-tree default keeps the
"source tree is self-contained, pytest from tree root" convention of the whole
project. The env override is needed for the UBSan cross-run (D6) and costs one
line.

**Acceptance.**
- D1 `nm` check passes; wrapper has explicit argtypes/restype on every symbol.
- `conftest.py` provides `soundforge_lib` fixture (cached `CDLL`) honoring
  `SOUNDFORGE_LIB_PATH`; parity tests alone use it (the 12 legacy tests
  untouched).
- Lifecycle helper ordering documented and enforced by context managers:
  `project → queue → engine` create, destroy in reverse; `engine` must be
  destroyed before its queue and before its project (mirrors the G4 caller
  lifetime hazard — the C++ side already guards destroy order in its tests).

### 3.4 D4 — Parity harness: what runs where

**Decision.** Three layers, one suite:

- **Authority:** `libsoundforge.so` via ctypes — the real C++ engine.
- **Reference:** `reference/engine.py` — pure-Python computation of the same
  deterministic surfaces.
- **Harness:** `regression/engine_parity.py` — signal generators + compare
  helpers with the shared tolerances; `tests/python_tests/test_engine_parity.py`
  wires fixtures → both sides → compare. (The `regression` package hosts the
  reusable comparison machinery; the pytest file stays a thin parametrized
  shell.)

Deterministic surface order (P1 → P2 → P3):
1. version/schema compatibility lockstep,
2. schema validate lockstep (decision-level on the shared fixture corpus),
3. migration lockstep (dict-equal results),
4. project JSON round-trip (semantic equality),
5. audio: fixed-gain chain → `tick` → `meter_json`/captured-block parity.

Audio parity mechanics (P2): each case builds its chain **as project JSON in
Python** (the reference needs the same doc anyway), feeds it to the C++ side
via `sf_project_from_json`, creates queue + engine, configures
(`sample_rate=48000, channels=2, max_block_frames=256` — any valid value;
deterministic), `set_output("out")`, `start(0)` (no pacer), then drives
deterministic `tick(...)` calls. The ctypes `read` callback fills L/R from the
**same Python signal generator** the reference uses (`dc_source(dc)` /
`sine_source(freq)`); a `write` callback captures rendered blocks for
block-level comparison. One fresh engine session **per test case** — the
engine/runner are one-shot, and no test shares engine state across cases
(mirrors the G5 two-session doctrine).

**Rationale.**
- Running the *real* engine makes G6 regression evidence (not a reimplementation
  pretending to be the engine); the Python reference is the *expectation*, and
  both derive from one Python-hosted signal source, so divergence can only
  come from the engine's own processing — exactly what parity must detect.
- Decision-level validate parity is the honest contract: native validation is
  the structural mirror in `schema.cpp`, Python validation is jsonschema on the
  canonical file. They are deliberately two implementations of one spec; G6
  pins them together on the shared corpus (the G1-era `dspPresetRef` lockstep
  already depends on this). Message text is documented as non-compared.
- Migrate parity is dict-equality **including** `engineVersion`: both sides
  stamp the same gate version (`0.1.0-g5` through P3, `0.1.0-g6` from P4 —
  bumped in one commit, so equality holds at every gate checkpoint).

**Acceptance.**
- Every parity case has a deterministic oracle (exact float anchors or
  reference-computed values) and no sleeps/barriers: `tick` is documented
  deterministic (no wall clock), the suite completes in seconds.
- Anchors (mirrored from the G5 fixture math, `test_audio_engine.cpp:931–945`
  and D5): chain `src(-6 dB)→out(0 dB)`, output `out` → block samples and
  meter `truePeakLinear` **0.501187** (`10^(-6/20)`); chain
  `src(-6)→out(+6)` → **1.0** (`0.501187 × 10^(+6/20) = 0.501187 × 1.995262`);
  chain `src(+6)→out(0)` → **1.995262** with `clipped == true`; silence path
  (`io` NULL) → `truePeakLinear == null` for both channels.
- Tolerances: meter `|native − reference| ≤ 1e-3` (same as unit tests); block
  samples `≤ 1e-5` relative (`1e-5 × (1 + |ref|)`); JSON compare is
  dict-semantic (order-insensitive).

### 3.5 D5 — Reference engine scope: minimal-but-honest, chain-gain surfaces only

**Decision.** `reference/engine.py` computes exactly:
- signal-graph topo order (asserts chain shape; cycles are already rejected by
  validate — the reference refuses non-chain graphs with `ValueError` and a
  fixture-driven reason);
- per-node linear gain: `10^(gainDb/20)` from `node.mixer.gainDb`
  (default `0.0` when absent; `mute` → 0.0);
- input block generation via the shared signal source (DC const or sine at
  fixed frequency/phase over N frames);
- per-node gain applied in topo order (render_plan.cpp:161–162 semantics: every
  node block, including the source's first block, is gained; output = target
  node's block);
- reference meter: max abs over the rendered output block's L/R (exact for DC;
  used for **block** parity only for sine — G6-1).

Deliberately **out** of the reference: TruePeak 4× oversample FIR
reimplementation, pan, mixer math, multi-source mixing, power/acoustics —
anything that would grow into the deferred NumPy-grade engine. The meter-parity
surface is therefore **DC-only** (true peak of a constant == its amplitude,
interpolation-free), while output-**block** parity covers fixed-frequency
sine sweep samples exactly (pure gain chain, float64 reference vs float32
engine at 1e-5 relative).

**Rationale.** "Minimal-but-honest": the reference must be *provably correct*
by inspection and exact on its surfaces, not a second engine to debug. DC meter
parity needs no FIR; sine is confined to block parity where the math is a
gain product per sample. Everything richer is a documented residual (G6-1) that
a later NumPy/deferred gate can grow without breaking the parity contract.

**Acceptance.**
- For any chain fixture in the validated corpus, reference output equals the
  engine output within the D4 tolerances (block 1e-5 rel; meter 1e-3 for DC).
- Non-chain graphs raise `ValueError` in the reference with the fixture reason
  (no silent wrong math).
- Reference `__init__.py`/docstring documents the boundary in one paragraph.

### 3.6 D6 — Parity runs against **both** build trees: reg primary, UBSan required

**Decision.** The shared lib builds in **both** `native/build` (reg) and
`native/build-asan` (UBSan — verified: `-fsanitize=undefined
-fno-sanitize-recover=all`; there is **no ASan** in this tree). The gate
requires the parity suite green against **both**:

```python
python3 -m pytest tests/python_tests -q                                   # reg .so (default path)
SOUNDFORGE_LIB_PATH=$PWD/native/build-asan/libsoundforge.so \
  python3 -m pytest tests/python_tests -q                                 # UBSan .so (env override)
```

**Rationale.**
- The G4/G5 doctrine keeps **both** builds green; parity is Python-side
  evidence over the same C++ code, so it should be green on the same two
  builds. Deterministic: identical inputs → identical results; a genuinely new
  UBSan finding aborts loudly in-process (`-fno-sanitize-recover=all`) → a
  red parity run, never a silent miss.
- **Why UBSan dlopen is safe here (and ASan would not be):** ASan requires its
  runtime to be first in the loader list (`LD_PRELOAD`) — that is the classic
  "ASan runtime does not come first" failure mode. This tree has **no ASan**;
  libubsan loads fine as a `DT_NEEDED` dependency of a dlopened library into a
  non-instrumented process (no ordering requirement). The Python process itself
  is not instrumented, so no interceptor clash. TSan (`build-tsan` exists) is
  explicitly **excluded**: TSan-instrumented libraries loaded into a non-TSan
  process are unsupported, and the engine's threading is already covered by the
  C++ unit suite (G6-7).
- Determinism guard: the parity code paths are the same code the unit suite
  already runs under UBSan; the only new Python-side code is ctypes glue (not
  instrumented). Risk of a UBSan-only divergence is ≈ nil; if the environment
  still shows dlopen friction at P1, the documented degrade is
  "UBSan tree builds the .so; parity green against reg only" — recorded in
  RELEASE_NOTES_G6.md, with the shared-lib contract itself unchanged.

**Acceptance.**
- Both trees produce `libsoundforge.so` (P1).
- Both invocations above pass at every phase checkpoint; counts equal.
- `build-tsan` is never loaded by pytest (no test references it).

---

## 4. Schema, Wire & Version Impact

### 4.1 Wire delta — **none**

G6 adds **no** document key, **no** ABI/header change, **no** error code, **no**
migration, **no** schema change. `schemaVersion` stays **2**;
`project_schema.json` byte-identical to `schema_golden_v2.json`; both SHA-256
pins untouched; the §7 drift `diff` must remain empty. **This is a deliberate
posture:** G6 is a host-side Python testability gate; the native contract is
consumed as-is.

### 4.2 Version stamp

G6 lands **`0.1.0-g6`** across **all seven** surfaces (P4, one commit; the
pyproject `0.1.0-g0` staleness is fixed here):

| # | Surface | Edit |
|---|---|---|
| 1 | `native/include/soundforge/sf_version.h` | `SF_ENGINE_VERSION_SUFFIX "-g6"` (+ one-line gate-history comment) |
| 2 | `native/CMakeLists.txt` | `SF_BUILD_VERSION "0.1.0-g6"` |
| 3 | `native/src/core/version.cpp` | fallback `SF_VERSION_STRING "0.1.0-g6"` |
| 4 | `native/src/core/version_gen.h.in` | comment-only bump (value flows from CMake) |
| 5 | `python/soundforge_py/__init__.py` | `__version__ = "0.1.0-g6"` |
| 6 | `python/soundforge_py/migrate.py` | `ENGINE_VERSION = "0.1.0-g6"` (logic untouched) |
| 7 | `python/pyproject.toml` | `version = "0.1.0-g6"` (+ description drops "stubs") |

Both reconfigured caches at P4 (same as G4/G5):

```bash
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g6 native/build
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g6 native/build-asan
```

Version **lockstep is tested** (not just asserted at DoD): the P1
`test_version_lockstep` case asserts `sf_engine_version()` (from the .so)
`== __version__` (Python package) — any future drift on either side fails
pytest. Not in scope (KEEP by design): `app/src/main/cpp/CMakeLists.txt`
keeps `0.1.0-g0` (Android static-review-only; see G6-5).

### 4.3 Diagnostics / logging deltas

None in the native layer (no TU edits). Python-side error surfacing is
documented in the wrapper: after a failed `sf_*` call, read
`sf_last_error(project_or_NULL)` / `sf_last_error_global()` via ctypes —
`sf_error_string` is header-inline and NOT exported (D1/D3). Parity tests
assert `sf_result` codes and error-text presence, not exact wording.

---

## 5. Phases (P0…P4)

Strict main-line order; each phase leaves the tree green (ctest reg + UBSan
both 314/314, pytest all green, drift empty) and is **one commit**. Expected
net-new pytest cases ≈ **14–18** (12 → ~26–30); growth is fine when
evidence-first. Native test count **unchanged** (314/314 — G6 edits no native
tests). JNI grep stays **26**.

### P0 — Plan commit (½ day)
- **Goal:** reviewable contract — this file, reviewed by orchestrator, then the
  final two-reviewer gate (oracle + security-reviewer, out-of-band) fills §10.
- **Files:** `docs/PLAN_G6.md`.
- **Acceptance:** §1–§10 present, internally consistent; D1–D6 explicit with
  rationale; no unresolved questions (Q1–Q5 in §9.3 answered at the gate).
- **Validation:** human review (no code).
- **Commit:** `plan(g6): PLAN_G6.md — python reference/regression parity (ctypes)`

### P1 — Host shared lib + ctypes loader + version/schema/migrate/project parity (1 day)
- **Goal:** libsoundforge.so exists in both trees; ctypes loads it; the four
  non-audio parity surfaces lockstep.
- **Files:**
  - `native/CMakeLists.txt` (EDIT — `SF_BUILD_HOST_SHARED` option +
    guarded `add_subdirectory(src/host)`).
  - `native/src/host/CMakeLists.txt` (NEW — D2 shared target).
  - `python/soundforge_py/engine.py` (NEW — D3 loader + bindings for version,
    schema, migration, project, string/error utils, command queue create/destroy).
  - `tests/python_tests/conftest.py` (NEW — `soundforge_lib` fixture honoring
    `SOUNDFORGE_LIB_PATH`; `repo_root`/`fixtures_dir`).
  - `tests/python_tests/test_engine_parity.py` (NEW — P1 subset: version
    lockstep, validate lockstep over the 5-fixture corpus, migrate lockstep
    v0→2/v1→2 dict-equality, project round-trip + getters).
- **Acceptance:**
  - Both trees build the `.so`; `nm -D` gate passes (D1); `sf_unit_tests`
    untouched; ctest 314/314 both trees; JNI 26.
  - Parity subset green against **both** reg and UBSan `.so` (D6 commands).
  - `test_version_lockstep`: `sf_engine_version() == __version__ == 0.1.0-g5`
    (pre-P4), `sf_schema_version() == SCHEMA_VERSION == 2`, compatibility
    truth table `(0..2 → 1, 3/-1 → 0)`.
  - Validate decision parity on the 5 fixtures (minimal_v2, signalgraph_v2,
    dspchain_v2, corrupt, graph_corrupt): accept/reject + error-presence equal.
  - Migrate dict-equality for v0→2 and v1→2 (incl. `engineVersion`).
  - Round-trip semantic equality for minimal_v2 + signalgraph_v2; getters
    lockstep with the fixture fields.
  - `import soundforge_py.engine` works with no `.so` present (lazy load);
    calling a binding without a build raises the D3 `RuntimeError`.
  - pytest ≈ 12 + ~8 = ~20; no fixed sleeps.
- **Validation:** host ctest (both trees) + both pytest invocations.
- **Commit:** `p1(g6): host libsoundforge.so + ctypes loader + version/schema/migrate/project parity`

### P2 — Audio-engine ctypes surface + tick/meter/block parity (1 day)
- **Goal:** the real engine's audio path runs from Python and matches the
  reference on DC meter + block samples.
- **Files:**
  - `python/soundforge_py/engine.py` (EDIT — audio surface: `AudioEngine`
    context manager, `CFUNCTYPE` io callbacks, `dc_source`/`sine_source` +
    block-capture sink helpers; error paths via `sf_last_error_global`).
  - `python/reference/engine.py` (NEW — D5 chain renderer: topo order, gain
    product, block render, DC meter, `ValueError` on non-chain graphs).
  - `python/regression/engine_parity.py` (NEW — tolerances 1e-3 meter / 1e-5
    rel block; compare helpers; signal generators).
  - `tests/python_tests/test_engine_parity.py` (EDIT — audio cases).
- **Acceptance:**
  - Chain fixtures built as project JSON in Python, `sf_project_from_json`,
    engine lifecycle per case (create→configure→set_output→start(0)→tick→
    stop→join→destroy; queue+project outlive engine, destroyed in reverse).
  - Anchors (D4): `(-6,0)` → meter + block **0.501187** @1e-3/1e-6;
    `(-6,+6)` → **1.0**; `(+6,0)` → **1.995262** + `clipped==true`; silence
    (`io` NULL) → `truePeakLinear == null`.
  - `reset_meters` keeps `blocksRendered` monotonic (not reset) — lockstep.
  - Sine (e.g. 1 kHz @ 48 kHz, N=256) block parity vs reference @1e-5 rel;
    meter parity DC-only (no sine meter compare — D5/G6-1).
  - Both pytest invocations green; pytest ≈ ~23.
- **Validation:** both pytest invocations + ctest (confirm no native change broke
  anything).
- **Commit:** `p2(g6): audio engine ctypes surface + tick/meter/block parity vs pure-python reference`

### P3 — Reference/regression package structure + parity expansion + docs (1 day)
- **Goal:** the layer is a usable surface, not a demo: package docs, wider
  deterministic matrix, save/open path coverage.
- **Files:**
  - `python/reference/engine.py` (EDIT — multi-node chains, mute handling,
    fixture-driven; docstring boundary).
  - `python/regression/engine_parity.py` (EDIT — parametrize helpers over the
    case table).
  - `tests/python_tests/test_engine_parity.py` (EDIT — matrix expansion:
    chains `(0,0)`, `(-6,0)`, `(-6,+6)`, `(+6,0)` at two block sizes;
    v0 fixture migrated by the C++ side then round-tripped through
    `sf_project_save_to_path`/`open_from_path` over `tmp_path` + re-validated
    by both validators; mono≡stereo channel agreement).
  - `python/README.md` (EDIT — layer description, run commands, loader
    contract, UBSan override one-liner).
  - `python/reference/__init__.py`, `python/regression/__init__.py` (EDIT —
    package docstrings).
- **Acceptance:** matrix green on both `.so` builds; save/open parity proves
  the on-disk path (not just in-memory JSON); README accurate to the shipped
  commands; pytest ≈ ~26–30 (exact count grows with the evidence, and the DoD
  records it).
- **Validation:** both pytest invocations + ctest + drift empty.
- **Commit:** `p3(g6): reference/regression packages + parity matrix expansion + python docs`

### P4 — Version 0.1.0-g6 + release notes + DoD sweep (1 day)
- **Goal:** full version sync (7 surfaces), release notes, final DoD.
- **Files:** all seven §4.2 surfaces + `docs/RELEASE_NOTES_G6.md` (NEW).
- **Acceptance:**
  - `-R Version` on both reconfigured caches reports `0.1.0-g6` (4/4).
  - `test_version_lockstep` now pins the C-ABI stamp == `__version__` ==
    `0.1.0-g6` (self-checking).
  - Full §7 matrix green; drift empty; JNI 26; `SF_E_*` 7; tree clean.
  - Release notes: deliverables, binding/shared-lib/parity decisions, anchors,
    residuals, P0–P4 commit list, versions.
  - Tag `g6-complete`.
- **Validation:** full §7 command matrix.
- **Commit:** `p4(g6): version 0.1.0-g6 + release notes + DoD sweep`

---

## 6. Test Plan

### 6.1 Native unit tests (`tests/unit/`, via ctest)

**Unchanged — 314/314 on reg and UBSan, both phases.** G6 edits no native
tests. The new shared target adds no test registration; `gtest_discover_tests`
output is identical. The parity suite is the G6 regression evidence *around*
the existing native coverage.

### 6.2 Python tests (`tests/python_tests/`)

| File | Cases |
|---|---|
| `test_schema_py.py` (KEEP) | 12 existing; untouched; golden SHA-256 pins intact. |
| `test_engine_parity.py` (NEW/EDIT) | P1: version lockstep (stamp==`__version__`, schema const, compat truth table); validate decision lockstep ×5 fixtures; migrate dict-equality v0→2/v1→2; project round-trip + getters ×2 fixtures. P2: audio anchors — `(-6,0)→0.501187`, `(-6,+6)→1.0`, `(+6,0)→1.995262+clip`, silence→null meters, `reset_meters` monotonic `blocksRendered`, sine block parity @1e-5 rel. P3: chain/block-size matrix, v0-migrate + save/open round trip over `tmp_path`, channel agreement. |

Expected totals: ~12 → ~20 (P1) → ~23 (P2) → ~26–30 (P3/P4). Growth is fine
when evidence-first; the DoD records the final number.

### 6.3 Android (static-only)

- `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` = **26**
  (G6 adds no JNI; re-verified every phase).
- `app/**` is KEEP — no Kotlin, no `jni_bridge.cpp` change, no app CMake change.
- New host target is guarded `AND NOT ANDROID`; nothing in the Android build
  graph changes (verified: app CMake never `add_subdirectory`s the native tree).

### 6.4 Verification command matrix (host)

```bash
# Builds + native suites (both trees)
cmake --build native/build -j2 && ctest --test-dir native/build --output-on-failure     # 314/314
cmake --build native/build-asan -j2 && ctest --test-dir native/build-asan --output-on-failure  # 314/314

# Shared lib artifact (P1+)
ls -l native/build/libsoundforge.so native/build-asan/libsoundforge.so
nm -D --defined-only native/build/libsoundforge.so | grep -c ' T sf_'        # >= 39
nm -D --defined-only native/build/libsoundforge.so | grep ' sf_error_string' # must be EMPTY (not exported)

# Parity + legacy pytest — reg default, UBSan override (D6)
python3 -m pytest tests/python_tests -q                                                   # ~26-30 pass
SOUNDFORGE_LIB_PATH=$PWD/native/build-asan/libsoundforge.so python3 -m pytest tests/python_tests -q  # same count

# Frozen-surface gates
diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json   # empty
grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' app/src/main/cpp/jni_bridge.cpp  # 26
grep -E 'SF_E_[A-Z_]+' native/include/soundforge/sf_types.h | grep '#define' | wc -l   # 7 (+ SF_OK)
git status --porcelain                                                             # clean at P4

# P4 reconfigure + version
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g6 native/build
cmake -U SF_BUILD_VERSION -DSF_BUILD_VERSION=0.1.0-g6 native/build-asan
ctest --test-dir native/build -R Version --output-on-failure                       # 4/4, 0.1.0-g6
```

---

## 7. Definition of Done (Gate G6)

All true on `main` (after P4):

1. **Builds:** ctest green on `native/build` (reg) **and** `native/build-asan`
   (UBSan) — **314/314 each, unchanged**; pytest green (~26–30);
   `libsoundforge.so` present in both trees; `nm -D` shows ≥39 `sf_` exports
   with default visibility and **no** `sf_error_string`.
2. **No drift:** `project_schema.json` byte-identical to `schema_golden_v2.json`;
   both pins untouched; `schema_golden_v1.json` untouched; schemaVersion **2**;
   no wire/ABI/header change; no new `SF_E_*` (still 7).
3. **Parity evidence:** the lockstep suite passes **against both reg and UBSan
   `.so` builds** (D6): version/schema lockstep, validate decision parity on
   the shared corpus, migrate dict-equality, project round-trip, and audio
   tick→meter/block parity at the documented anchors (0.501187 / 1.0 /
   1.995262+clip / null-silence) and tolerances (1e-3 meter, 1e-5 rel block).
4. **No new JNI / no Android:** `app/**` untouched; export grep **26**; host
   target guarded out of Android toolchains.
5. **Version stamp `0.1.0-g6`** on all **seven** §4.2 surfaces, both
   reconfigured caches, `-R Version` 4/4, and the C-ABI stamp == Python
   `__version__` (test-enforced).
6. **Native layer untouched:** zero edits under `native/src/**` except the two
   P4 version-string files; `tests/unit/**` untouched; `sf_unit_tests` link set
   unchanged.
7. **Docs:** this plan + `docs/RELEASE_NOTES_G6.md` (decisions, anchors,
   residuals, P0–P4 commits); tree clean; tag `g6-complete`.

---

## 8. Risks

| # | Risk | Sev | Mitigation |
|---|---|---|---|
| R1 | ctypes signature drift vs headers (wrong argtype/restype silently corrupts) | MED | Every binding declared from one header-annotated table with explicit `argtypes`/`restype` (no default conversions); parity suite doubles as ABI smoke test (deterministic compares catch garbage); `nm` gate catches missing/hidden exports. |
| R2 | `.so` missing on a source-only checkout → parity tests fail unhelpfully | LOW | Lazy load + D3 `RuntimeError` naming both search paths and the build hint; `import soundforge_py.engine` never fails; 12 legacy tests stay green without a native build. |
| R3 | UBSan-instrumented `.so` dlopen friction in Python | LOW | D6: this tree is UBSan-only (no ASan ordering problem); `-fno-sanitize-recover=all` makes UB a loud abort = red run. Degrade path documented (reg-only parity + UBSan compile coverage) if the environment proves otherwise at P1. |
| R4 | float64 reference vs float32 engine divergence exceeds tolerance | LOW | Relative block tolerance 1e-5 + DC/fixed-sine fixtures chosen away from denormals/phase edges; meter tolerance 1e-3 mirrors the native unit tests exactly. |
| R5 | Link-time surprise linking five statics into one `.so` demands a TU edit | LOW-MED | Pure link/packaging target over unchanged libs (D2); **whole-archive semantics mandated so all C-ABI objects are exported** (amendment); `stdc++fs` + `Threads::Threads` included; PIC already ON; the D1 `nm` gate catches an empty export table immediately. Any required TU edit escalates to orchestrator + oracle (asserted in §1.3), never a silent amendment. |
| R6 | Version drift between CMake cache stamp and Python `__version__` | LOW | Single P4 commit bumps all seven surfaces + two-cache reconfigure (G4/G5 precedent); `test_version_lockstep` makes drift a permanent test failure. |
| R7 | One-shot engine lifecycle mishandled in pytest (leaked queue/engine ordering) | LOW | One fresh session per case; context managers enforce project→queue→engine create / reverse destroy (mirrors G4 caller-lifetime hazard); no engine shared across cases. |
| R8 | Parity count growth or runtime creep | LOW | Suite is seconds-scale (≤512-frame ticks, 1–9 ticks/case like the unit tests); matrix bounded in P3; DoD records final count. |

---

## 9. Residuals & Open Questions

### 9.1 Prior residuals — disposition in G6

| # | Residual | Disposition in G6 |
|---|---|---|
| G4-1 | Real Android device | **STANDS** — unchanged (G6 adds no Android) |
| G4-2 | True peak not BS.1770-certified | **STANDS** — unchanged |
| G4-3 | Pacer timing best-effort | **STANDS** — unchanged (parity uses deterministic tick, never the pacer) |
| G4-7 | Independent two-reviewer gate | **RESOLVED** (G4/G5 precedent; G6 repeats the process on this plan) |
| G4-10 | Callback error paths | **STANDS** — unchanged; parity observes the documented callback contract via ctypes |
| G5-1 | G4-5 plan memory not hardened | **STANDS** — unchanged |
| G5-2 | Live retarget best-effort timing | **STANDS** — unchanged (parity exercises the static set_output path only) |
| G5-3 | Unknown target → valid plan rendering silence | **STANDS** — unchanged; parity's silence case (null io) is distinct from unknown-target silence |

### 9.2 New G6 residuals (post-gate)

| # | Residual | Sev | Disposition |
|---|---|---|---|
| G6-1 | Reference engine is chain-gain-only (no pan/mixer/multi-source/TruePeak-FIR); sine meter parity not covered | LOW | Boundary documented in `reference/engine.py` + release notes. Grows into the deferred NumPy-grade engine later without breaking the parity contract (DC meter + block parity stay exact). |
| G6-2 | ctypes wrapper covers ≈39 of ≈60 exports; diagnostics, geometry, graph mutators, `apply_batch`, standalone queue runner, enqueue/dequeue unwrapped | LOW | Explicit scope boundary; later gates add tables to `engine.py`. Parity only wraps what parity exercises. |
| G6-3 | No pip packaging/entry points/install target; pytest runs from the source tree | LOW | Matches project convention; a packaging gate can add `pip install -e python` + console scripts later. |
| G6-4 | `python/data_import/` remains a stub | LOW | Belongs to the native measurement-import lane, not G6. |
| G6-5 | `app/src/main/cpp/CMakeLists.txt` keeps stale `0.1.0-g0` stamp | LOW | App metadata is Android-static-review-only; host engine stamp governs G6. Sync deferred to a future Android-facing gate. |
| G6-6 | Python `migrate.py`/`schema.py` remain mirrors, not forks | LOW | Lockstep is *test-enforced* by the P1 parity cases; any future spec change must land both sides in one commit or the suite fails. |
| G6-7 | TSan build (`build-tsan`) excluded from parity | LOW | TSan-instrumented libraries cannot be dlopened into an uninstrumented process; engine threading already unit-covered. |

### 9.3 Open questions — for the final two-reviewer gate (to be resolved in §10)

- **Q1 (binding):** endorse ctypes (D1)? Any reviewer-identified case where
  cffi/pybind11 would be decisive?
- **Q2 (shared lib):** endorse `src/host/` + `SF_BUILD_HOST_SHARED` guard +
  build-tree path contract with `SOUNDFORGE_LIB_PATH` override (D2/D3)? Any
  objection to no-install-target / no-`install(TARGETS)`?
- **Q3 (UBSan parity):** endorse the D6 posture — parity REQUIRED against both
  reg and UBSan `.so`, degrade path documented? Confirm reviewer agreement that
  `build-asan` (UBSan only) makes dlopen safe, and that TSan stays excluded.
- **Q4 (reference scope):** endorse DC-only meter parity + sine confined to
  block parity (D5)? Any reviewer objection to the reference being
  chain-gain-only (G6-1)?
- **Q5 (app stamp):** endorse leaving `app/src/main/cpp/CMakeLists.txt` at
  `0.1.0-g0` (G6-5) as a documented KEEP rather than adding an 8th surface?

---

## 10. Gate Record Template (filled after the two-reviewer gate)

### 10.1 P0 gate — oracle verdict (out-of-band, TBD)

| ID | Sev | Finding | Amendment landed |
|---|---|---|---|
| ORC-G6-01 | — | *TBD* | |
| ORC-G6-02 | — | *TBD* | |
| … | | | |

### 10.2 P0 gate — security reviewer verdict (out-of-band, TBD)

| ID | Sev | Finding | Amendment landed |
|---|---|---|---|
| SEC-G6-01 | — | *TBD* | |
| SEC-G6-02 | — | *TBD* | |
| … | | | |

### 10.3 Q&A table — two-reviewer gate

| Question | Oracle verdict | Security verdict | Landed in |
|---|---|---|---|
| Q1 binding = ctypes | *TBD* | *TBD* | |
| Q2 shared-lib guard + path contract | *TBD* | *TBD* | |
| Q3 UBSan parity posture | *TBD* | *TBD* | |
| Q4 reference scope (DC-only meter) | *TBD* | *TBD* | |
| Q5 app stamp KEEP | *TBD* | *TBD* | |

---

*End of PLAN_G6.md (draft — scratch, uncommitted).*