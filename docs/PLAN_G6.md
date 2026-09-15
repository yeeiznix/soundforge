# SoundForge — Gate G6 Plan: Python Reference/Regression Parity —
# ctypes Layer over a Host Shared Library + Pure-Python Reference + Lockstep Suite

> **Status: AMENDED — two-reviewer gate complete (oracle ORC-G6-01..17 +
> security SEC-G6-01..10 landed, 2026-09-15); P1-ready.**
> This document is the phase-0 plan contract for Gate G6. Reviewed and amendments
> applied per the gate process (final two-reviewer gate: oracle +
> security-reviewer, out-of-band) before P1. The plan specifies the
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
| **Host shared library** | `native/CMakeLists.txt` (EDIT), `native/src/host/CMakeLists.txt` (NEW), `native/src/host/host_stub.cpp` (NEW, generated 1-line anchor TU) | New `SF_BUILD_HOST_SHARED` option (default ON on non-Android) gating a SHARED target `soundforge` → `libsoundforge.so`, linking the five existing statics + `Threads::Threads` + `stdc++fs`, plus a 1-line anchor TU (CMake requires ≥1 source on a buildable library target — ORC-G6-01) and a pinned `LIBRARY_OUTPUT_DIRECTORY` → build root (ORC-G6-03). Default visibility so all `extern "C"` exports are present. Built in **both** `native/build` and `native/build-asan` (UBSan) trees. Does not touch the static unit-test target, does not touch `app/src/main/cpp/CMakeLists.txt` (which never `add_subdirectory`s the native tree — verified). |
| **ctypes loader + bindings** | `python/soundforge_py/engine.py` (NEW P1, EDIT P2) | Stdlib `ctypes.CDLL` loader with a documented path contract (env `SOUNDFORGE_LIB_PATH` → `native/build/libsoundforge.so`), lazy load so `import soundforge_py.engine` never fails without a build, opened with explicit `RTLD_LOCAL` (SEC-G6-02); env override must be absolute, `realpath`-canonicalized, `isfile`-checked, non-absolute values rejected (SEC-G6-01). Bindings for ~39 symbols: version (3), schema (1), migration (1), project (17), string/error utils (3), command queue (2), audio engine (11). High-level Pythonic helpers + `AudioEngine`/`Project` context managers. |
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
  except `version.cpp` (P4 fallback string), `version_gen.h.in` (P4 comment),
  and `src/host/host_stub.cpp` (P1 — 1-line anchor TU with **no engine logic**;
  CMake requires ≥1 source on a buildable library target, ORC-G6-01);
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

Same spirit as G0–G5 with a Python-specific adaptation: new native files are
**build config only** — `src/host/CMakeLists.txt` plus a 1-line anchor TU
`src/host/host_stub.cpp` (no engine logic; the "native layer consumed
unchanged" posture from §1.3 holds explicitly); new Python files
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
- `nm -D --defined-only native/build/libsoundforge.so | grep ' T sf_'` includes
  **every** symbol in the §3.3 wrapped-surface table (explicit name list, not
  a bare count — ORC-G6-13: 65 `T sf_` exports exist, so ≥39 passes even if a
  wrapped symbol is missing) and **does not** show `sf_error_string`
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
find_package(Threads REQUIRED)          # imported target is directory-scoped —
                                        # does NOT leak from src/core|graph|audio (ORC-G6-02)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/host_stub.cpp"
     CONTENT "/* G6 host shared-lib anchor TU — no engine logic */\n")
add_library(soundforge SHARED "${CMAKE_CURRENT_BINARY_DIR}/host_stub.cpp")  # ≥1 source required (ORC-G6-01)
target_link_libraries(soundforge PRIVATE
    -Wl,--whole-archive                                    # require ALL objects from the
    sfcore sfgraph sfdsp sfmeasure sfaudio                 # five statics (below)
    -Wl,--no-whole-archive
    Threads::Threads stdc++fs)
set_target_properties(soundforge PROPERTIES
    OUTPUT_NAME soundforge
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"         # .so lands at build/libsoundforge.so (ORC-G6-03)
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET default   # explicit: keep default visibility
    VISIBILITY_INLINES_HIDDEN OFF)
```

> **ORC-G6 pre-gate amendment (D2, orchestrator + oracle review):**
> `-Wl,--whole-archive` is **mandatory**. A shared library with no source files
> has no undefined symbols of its own, and GNU/LLVM ld extract from a static
> archive **only the object files that resolve an undefined symbol** — probe
> confirmed: whole-archive **off** → **0** `T sf_` exports; whole-archive **on**
> → **65** (ORC-G6-17). The `$<LINK_LIBRARY:WHOLE_ARCHIVE,...>` genex (CMake ≥
> 3.24) and the plain `-Wl,--whole-archive … --no-whole-archive` form both work
> (65 exports); the `$<TARGET_FILE:…>` + `--push/--pop-state` form fails at
> generate time. No link-level duplicate-definition hazard was reproducible —
> duplicates across statics either fail the .so link loudly or are deduped;
> treat that as link-time-only, not a silent surface. The D1 `nm -D` gate
> (explicit wrapped-name list, §3.3) is the acceptance check either way.

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
  produce `libsoundforge.so` **at the build root** (pinned
  `LIBRARY_OUTPUT_DIRECTORY` — `native/build/libsoundforge.so` and
  `native/build-asan/libsoundforge.so`; ORC-G6-03) (P1 acceptance lists `ls`
  of both trees).
- `target_link_libraries` uses **PRIVATE** — the oracle's corrected form; a
  leaf shared target never propagates the statics (PUBLIC harmless but leaky).
- `sf_unit_tests` target/link set is byte-unchanged (no CMake edit touches
  `tests/unit/CMakeLists.txt`); ctest counts stay 314/314 on both trees.
- Android app CMake untouched; JNI grep stays 26.
- `nm -D` shows the C-ABI symbols exported with **default** visibility
  (D1 acceptance).

### 3.3 D3 — Loader path contract and wrapped surface

**Decision.** `soundforge_py/engine.py` resolves the library in this order:

1. `$SOUNDFORGE_LIB_PATH` — the *named* mechanism for pointing pytest at
   `native/build-asan/libsoundforge.so` (D6). **Must be an absolute path** —
   non-absolute values are rejected with the D3 `RuntimeError` (closes the
   CWD-relative hijack variant; SEC-G6-01). The value is `realpath`-canonicalized
   and `isfile`-checked before dlopen; on failure the `RuntimeError` names both
   candidate paths + the `cmake --build` hint.
2. `REPO_ROOT / "native" / "build" / "libsoundforge.so"` — default (reg build).
3. Otherwise `RuntimeError` naming both tried paths and a
   `cmake --build native/build` hint.

Loading is **lazy** (function-level, cached in a module singleton): importing
`soundforge_py.engine` never fails without a build, so the 12 legacy tests and
any non-parity import path stay green on a source-only checkout. The CDLL is
opened with **explicit `RTLD_LOCAL`** — deterministic on any platform, and the
.so's internal mangled C++ symbols can never interpose on (or be interposed by)
other DSOs in the pytest process (SEC-G6-02). Never `RTLD_GLOBAL`. Wrapped
surface (≈39 symbols, header → functions):

| Header | Wrapped | Notes |
|---|---|---|
| `sf_version.h` | `sf_engine_version`, `sf_schema_version`, `sf_is_compatible` | lockstep vs `__version__`/`SCHEMA_VERSION` |
| `sf_schema.h` | `sf_validate_project_json` | sized err buffer (4 KB); decision-level result; JSON entry via the `_as_bytes` helper (SEC-G6-03) |
| `sf_migration.h` | `sf_migrate_json` | in-place buffer sized `max(8*len(raw), 1<<16)` (or NOMEM-retry doubling — ORC-G6-08: pretty output ≈15.6× minified); `*inout_len` excludes NUL, cap = buffer size; `_as_bytes` helper (SEC-G6-03) |
| `sf_project.h` | create/destroy/clone/to_json/from_json/save_to_path/open_from_path + 4 getters + rename/venue/scene mutators + health_check (17 total) | `char** out_json` via `POINTER(c_char_p)` — free via `_take_string` (bytes copied with `string_at`, out ptr zeroed, `sf_free_string` exactly once; SEC-G6-04); restype/cast trap: `out.contents` on `c_char_p` raises `AttributeError` — free `ctypes.cast(out, c_void_p)` (ORC-G6-14); getters via `c_char_p.value` (no free — static/internal storage) |
| utils | `sf_free_string`, `sf_last_error`, `sf_last_error_global` | error text path: `sf_error_string` NOT exported (header-inline); error strings valid only until the next error-set, read same-thread immediately (SEC-G6-04) |
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
- D1 `nm` check passes (explicit wrapped-name list); wrapper has explicit
  argtypes/restype on every symbol.
- One `_as_bytes(s)` helper derives byte length from the encoded bytes
  (`len(s.encode("utf-8"))`, never char count / `sys.getsizeof`) for **all**
  JSON-entry calls — `sf_validate_project_json`, `sf_project_from_json`,
  `sf_migrate_json` (SEC-G6-03; a char-count length on non-ASCII input is an
  OOB read in the .so).
- One `_take_string(out_ptr)` helper is the **only** code path that frees
  native memory (SEC-G6-04); raw `char*` never escapes the wrapper.
- Wrapper classes have **no `__del__`/finalizers** — destruction is explicit
  via context managers only; `AudioEngine` holds strong Python refs to its
  queue and project for its lifetime (SEC-G6-06).
- `conftest.py` provides `soundforge_lib` fixture (session-scoped, cached
  `CDLL`) honoring `SOUNDFORGE_LIB_PATH` (absolute, canonicalized); parity
  tests alone use it (the 12 legacy tests untouched). Conftest computes
  `repo_root` file-relatively (`Path(__file__).resolve().parents[...]`) and
  avoids importing `engine.py` at module time — the lazy-load contract applies
  to conftest too, keeping the legacy 12 green on source-only checkouts
  (SEC-G6-08).
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
3. migration lockstep (**normalized** dict-equality — UUID/RFC3339 timestamp
   fields masked on both sides, auditLog as ordered tuples, `engineVersion`
   kept strict; ORC-G6-04 — raw equality is impossible: both migrators inject
   fresh UUIDs + wall-clock ms),
4. project JSON round-trip (**canonical-form idempotence**: pass1 == pass2 ==
   pass3 of `to_json(from_json(X))`; do not compare raw fixture bytes to
   emitted JSON — `dspPresetRef:null`/`gainDb:0.0` emission differs per
   fixture; ORC-G6-06),
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

**Byte identity (SEC-G6-07):** parity serializes the project document **once**
as Python bytes and feeds those exact bytes to `sf_project_from_json` /
`sf_validate_project_json` / `sf_migrate_json` and to the reference
(`json.loads` on the same bytes) — no re-dump of a parsed dict for the native
side, which could mask a UTF-8/escaping differential. Fixture files reach
`sf_validate_project_json` as raw file bytes.

**Callback contract (SEC-G6-05, all binding):** callbacks copy data **within
the call** (read fills per-channel `(c_float*frames)` views; write copies out
immediately) and never retain/dereference engine-owned pointers after return.
Bodies are wrapped in `try/except` — on exception, record in a per-session
error list and return `SF_E_IO` (never let it escape: ctypes would print and
return 0 == SF_OK, treating half-filled input as valid). Assert `channels == 2`
and `frames >= 1` at the top of each callback. The `CFUNCTYPE` instances are
kept alive as attributes of the `AudioEngine` wrapper for the engine's lifetime
(the native side stores raw function pointers; dropping the CFUNCTYPE is a
dangling pointer on the next tick). Parity teardown asserts zero recorded
callback errors.

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
- Migrate parity is **normalized** dict-equality: UUID-v4 and RFC3339 string
  fields (`project.createdAt/modifiedAt`, `venue.id`, `scene.id`,
  `auditLog[].ts`) are masked on both sides — both migrators inject fresh
  UUIDs + wall-clock ms, so raw equality is unimplementable (ORC-G6-04 probe:
  NO on every fixture). `engineVersion` is kept **strict** in the compare —
  both sides stamp the same gate version and that is the real lockstep signal
  (`0.1.0-g5` through P3, `0.1.0-g6` from P4 — bumped in one commit).

**Acceptance.**
- Every parity case has a deterministic oracle (exact float anchors or
  reference-computed values) and no sleeps/barriers: `tick` is documented
  deterministic (no wall clock), the suite completes in seconds.
- Anchors (mirrored from the G5 fixture math, `test_audio_engine.cpp:931–945`
  and D5; oracle re-verified via ctypes, prime or no prime): chain
  `src(-6 dB)→out(0 dB)`, output `out` → block samples and meter
  `truePeakLinear` **0.501187** (`10^(-6/20)`); chain `src(-6)→out(+6)` →
  **1.0** (`0.501187 × 10^(+6/20) = 0.501187 × 1.995262`); chain
  `src(+6)→out(0)` → **1.995262** with `clipped == true` (probe: clipped true
  for (+6,0), false for (-6,0)). **Silence path** (`io` NULL) →
  `truePeakLinear == 0.0` and `truePeakDb == null` for both channels — the
  frozen engine emits `[0.0,0.0]` linear, `[null,null]` dB (ORC-G6-05, probe;
  NOT `null` linear).
- Tolerances, per surface (single source of truth in
  `regression/engine_parity.py`, ORC-G6-10): meter `|native − reference| ≤ 1e-3`
  (same as unit tests, DC only); **DC-anchor block samples ≤ 1e-6 absolute**
  (mirrors `test_audio_engine.cpp:941–942`); **sine block samples ≤ 1e-5
  relative** (`1e-5 × (1 + |ref|)` — float64 reference vs float32 engine);
  JSON compare is dict-semantic (order-insensitive).

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
  non-instrumented process (no ordering requirement — oracle probe verified
  `build-asan` is UBSan-only). The Python process itself is not instrumented,
  so no interceptor clash. TSan (`build-tsan` exists) is explicitly
  **excluded**: TSan-instrumented libraries loaded into a non-TSan process are
  unsupported, and the engine's threading is already covered by the C++ unit
  suite (G6-7). The .so is loaded RTLD_LOCAL (SEC-G6-02), so its mangled C++
  interior can never interpose on other DSOs. **Future ASan note:** if the tree
  ever gains real ASan, the "runtime must be first" rule reapplies for the
  Python process — keep the degrade-path wording (SEC-G6-02).
- **P1 confirmation (Q3):** P1 must confirm `SOUNDFORGE_LIB_PATH=$PWD/native/build-asan/libsoundforge.so`
  actually loads the UBSan build (e.g. `sf_engine_version()` smoke) **before**
  the documented degrade path may be relied on.
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
| 2 | `native/CMakeLists.txt` | `SF_BUILD_VERSION "0.1.0-g6"` + the adjacent comment-history line and `g5+build.123` example (ORC-G6-12 — G5 precedent ORC-G5-06) |
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
  - `native/src/host/CMakeLists.txt` (NEW — D2 shared target, corrected form:
    anchor TU + `find_package(Threads)` + `LIBRARY_OUTPUT_DIRECTORY`).
  - `native/src/host/host_stub.cpp` (NEW — 1-line anchor TU, no engine logic;
    generated by `file(GENERATE …)` from the CMake file, not committed).
  - `python/soundforge_py/engine.py` (NEW — D3 loader + bindings for version,
    schema, migration, project, string/error utils, command queue create/destroy).
  - `tests/python_tests/conftest.py` (NEW — `soundforge_lib` fixture honoring
    `SOUNDFORGE_LIB_PATH`; `repo_root`/`fixtures_dir`).
  - `tests/python_tests/test_engine_parity.py` (NEW — P1 subset: version
    lockstep, validate lockstep over the 5-fixture corpus, migrate lockstep
    v0→2/v1→2 dict-equality, project round-trip + getters).
- **Acceptance:**
  - Both trees build the `.so`; `nm -D` gate passes (D1, explicit wrapped-name
    list); `sf_unit_tests` untouched; ctest 314/314 both trees; JNI 26.
  - Parity subset green against **both** reg and UBSan `.so` (D6 commands);
    P1 confirms the env override loads the UBSan build before relying on any
    degrade path (Q3).
  - `test_version_lockstep`: `sf_engine_version() == __version__ == 0.1.0-g5`
    (pre-P4), `sf_schema_version() == SCHEMA_VERSION == 2`, compatibility
    truth table `(0..2 → 1, 3/-1 → 0)`.
  - Validate decision parity on the 5 fixtures (minimal_v2, signalgraph_v2,
    dspchain_v2, corrupt, graph_corrupt): accept/reject + error-presence equal
    (ORC-G6-16 probe: parity holds on all of them, incl. the full corpus).
  - Migrate **normalized** equality for v0→2 and v1→2 (UUID/RFC3339 masked,
    auditLog ordered tuples, `engineVersion` strict — ORC-G6-04).
  - Round-trip **canonical-form idempotence** on minimal_v2 + signalgraph_v2
    (pass1 == pass2 == pass3 of `to_json(from_json(X))`; NOT raw fixture bytes
    — ORC-G6-06); `dspchain_v2` excluded from round-trip (native codec drops
    `signalGraph.mixers` — known-lossy, ORC-G6-07, residual G6-9). Getters
    lockstep with fixture fields; `sf_project_get_engine_version` compares to
    the **fixture's own `engineVersion`** (e.g. `0.1.0-g1`), only
    `sf_engine_version()` compares to `__version__` (ORC-G6-09).
  - One non-ASCII parity case (`café` in a venue/scene name): round-trips
    `from_json`/`to_json` and validates on both sides — proves byte-length
    discipline (SEC-G6-03).
  - `import soundforge_py.engine` works with no `.so` present (lazy load);
    calling a binding without a build raises the D3 `RuntimeError`.
  - pytest ≈ 12 + ~9 = ~21; no fixed sleeps.
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
  - Anchors (D4): `(-6,0)` → meter + block **0.501187** (meter |Δ| ≤ 1e-3,
    block ≤ 1e-6 abs); `(-6,+6)` → **1.0**; `(+6,0)` → **1.995262** +
    `clipped==true`; silence (`io` NULL) → `truePeakLinear == 0.0` +
    `truePeakDb == null` (ORC-G6-05).
  - `reset_meters` keeps `blocksRendered` monotonic (not reset) — lockstep.
  - Sine (e.g. 1 kHz @ 48 kHz, N=256) block parity vs reference ≤ 1e-5 rel;
    meter parity DC-only (no sine meter compare — D5/G6-1).
  - Callback contract verified by construction: `try/except` bodies recording
    errors (teardown asserts zero), `channels == 2`/`frames >= 1` tripwires,
    CFUNCTYPE instances alive on the wrapper — parity teardown proves it
    (SEC-G6-05).
  - Lifecycle: no `__del__` on wrappers; context managers only; `AudioEngine`
    holds strong refs to queue + project (SEC-G6-06).
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
| `test_engine_parity.py` (NEW/EDIT) | P1: version lockstep (stamp==`__version__`, schema const, compat truth table); validate decision lockstep ×5 fixtures; migrate **normalized** equality v0→2/v1→2; project **canonical-form** round-trip + getters ×2 fixtures (+1 non-ASCII case). P2: audio anchors — `(-6,0)→0.501187`, `(-6,+6)→1.0`, `(+6,0)→1.995262+clip`, silence→`truePeakLinear 0.0`/`truePeakDb null`, `reset_meters` monotonic `blocksRendered`, sine block parity (≤1e-5 rel). P3: chain/block-size matrix, v0-migrate + save/open round trip over `tmp_path`, channel agreement. |

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
nm -D --defined-only native/build/libsoundforge.so | grep ' T sf_'           # incl. EVERY §3.3 wrapped symbol (65 total; explicit name list, not a bare ≥39 count — ORC-G6-13)
nm -D --defined-only native/build/libsoundforge.so | grep ' sf_error_string' # must be EMPTY (not exported)
nm -D --defined-only native/build-asan/libsoundforge.so | grep -c ' T sf_'   # 65 — UBSan artifact exports the same surface

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
   `libsoundforge.so` present at the build root of both trees; `nm -D` shows
   **all** §3.3 wrapped symbols (65 exported) with default visibility and
   **no** `sf_error_string`.
2. **No drift:** `project_schema.json` byte-identical to `schema_golden_v2.json`;
   both pins untouched; `schema_golden_v1.json` untouched; schemaVersion **2**;
   no wire/ABI/header change; no new `SF_E_*` (still 7).
3. **Parity evidence:** the lockstep suite passes **against both reg and UBSan
   `.so` builds** (D6): version/schema lockstep, validate decision parity on
   the shared corpus, migrate **normalized** dict-equality (UUID/ts masked,
   `engineVersion` strict), project **canonical-form** round-trip, and audio
   tick→meter/block parity at the documented anchors (0.501187 / 1.0 /
   1.995262+clip / **0.0-linear+null-dB** silence) and per-surface tolerances
   (meter 1e-3; DC block ≤ 1e-6 abs; sine block ≤ 1e-5 rel).
4. **No new JNI / no Android:** `app/**` untouched; export grep **26**; host
   target guarded out of Android toolchains.
5. **Version stamp `0.1.0-g6`** on all **seven** §4.2 surfaces, both
   reconfigured caches, `-R Version` 4/4, and the C-ABI stamp == Python
   `__version__` (test-enforced).
6. **Native layer untouched:** zero edits under `native/src/**` except the two
   P4 version-string files and the P1 `src/host/` addition (CMakeLists.txt +
   generated 1-line anchor TU — build config only, no engine logic);
   `tests/unit/**` untouched; `sf_unit_tests` link set unchanged.
7. **Docs:** this plan + `docs/RELEASE_NOTES_G6.md` (decisions, anchors,
   residuals, P0–P4 commits); tree clean; tag `g6-complete`.

---

## 8. Risks

| # | Risk | Sev | Mitigation |
|---|---|---|---|
| R1 | ctypes signature drift vs headers (wrong argtype/restype silently corrupts) | MED | Every binding declared from one header-annotated table with explicit `argtypes`/`restype` (no default conversions); parity suite doubles as ABI smoke test (deterministic compares catch garbage); `nm` gate catches missing/hidden exports. |
| R2 | `.so` missing on a source-only checkout → parity tests fail unhelpfully | LOW | Lazy load + D3 `RuntimeError` naming both search paths and the build hint; `import soundforge_py.engine` never fails; 12 legacy tests stay green without a native build. |
| R3 | UBSan-instrumented `.so` dlopen friction in Python | LOW | D6: this tree is UBSan-only (no ASan ordering problem — oracle-verified); `-fno-sanitize-recover=all` makes UB a loud abort = red run. P1 must confirm the `SOUNDFORGE_LIB_PATH` override actually loads the UBSan build before the documented degrade path (reg-only parity + UBSan compile coverage) may be relied on. |
| R4 | float64 reference vs float32 engine divergence exceeds tolerance | LOW | Relative block tolerance 1e-5 + DC/fixed-sine fixtures chosen away from denormals/phase edges; meter tolerance 1e-3 mirrors the native unit tests exactly. |
| R5 | Link-time surprise linking five statics into one `.so` demands a TU edit | LOW-MED | Pure link/packaging target over unchanged libs (D2); **whole-archive semantics mandated and probe-verified** — off → 0 exports, on → 65 (ORC-G6-17); no duplicate-definition link hazard reproducible; the corrected CMake form (anchor TU, `find_package(Threads)`, `LIBRARY_OUTPUT_DIRECTORY`) is specified in D2; `stdc++fs` + `Threads::Threads` included; PIC already ON; the D1 `nm` gate catches an empty export table immediately. Any required TU edit escalates to orchestrator + oracle (asserted in §1.3), never a silent amendment. |
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
| G4-10 | Callback error paths | **STANDS** — unchanged; parity observes the documented callback contract via ctypes (SEC-G6-05: try/except → `SF_E_IO`, per-session error list; callbacks run on the caller thread only — tick-only, no pacer — so the header's lock-free/alloc-free advisory is not exercised from Python; new residual G6-8) |
| G5-1 | G4-5 plan memory not hardened | **STANDS** — unchanged |
| G5-2 | Live retarget best-effort timing | **STANDS** — unchanged (parity exercises the static set_output path only) |
| G5-3 | Unknown target → valid plan rendering silence | **STANDS** — unchanged; parity's silence case (null io) is distinct from unknown-target silence |

### 9.2 New G6 residuals (post-gate)

| # | Residual | Sev | Disposition |
|---|---|---|---|
| G6-1 | Reference engine is chain-gain-only (no pan/mixer/multi-source/TruePeak-FIR); sine meter parity not covered | LOW | Boundary documented in `reference/engine.py` + release notes. Grows into the deferred NumPy-grade engine later without breaking the parity contract (DC meter + block parity stay exact). |
| G6-2 | ctypes wrapper covers ≈39 of ≈60 exports; diagnostics, geometry, graph mutators, `apply_batch`, standalone queue runner, enqueue/dequeue unwrapped | LOW | Explicit scope boundary; later gates add tables to `engine.py`. Parity only wraps what parity exercises. |
| G6-3 | No pip packaging/entry points/install target; pytest runs from the source tree | LOW | Matches project convention; a packaging gate can add `pip install -e python` + console scripts later. **SEC-G6-09:** before packaging/console scripts, re-review the `SOUNDFORGE_LIB_PATH` dlopen posture (SEC-G6-01) and `RTLD_LOCAL` choice — the loader would move from test-only to installed-library consumption. |
| G6-4 | `python/data_import/` remains a stub | LOW | Belongs to the native measurement-import lane, not G6. |
| G6-5 | `app/src/main/cpp/CMakeLists.txt` keeps stale `0.1.0-g0` stamp | LOW | App metadata is Android-static-review-only; host engine stamp governs G6. Sync deferred to a future Android-facing gate. |
| G6-6 | Python `migrate.py`/`schema.py` remain mirrors, not forks | LOW | Lockstep is *test-enforced* by the P1 parity cases; any future spec change must land both sides in one commit or the suite fails. |
| G6-7 | TSan build (`build-tsan`) excluded from parity | LOW | TSan-instrumented libraries cannot be dlopened into an uninstrumented process; engine threading already unit-covered. |
| G6-8 | Parity callbacks bypass the header's lock-free/alloc-free advisory (sf_audio_engine.h:49–51) | LOW | CPython callbacks allocate a frame + take the GIL. Safe here: parity uses `start(0)` (no pacer), callbacks run on the calling Python thread (ORC-G6-11, SEC-G6-05). A future gate wrapping PACE must treat callbacks as native-thread + GIL-safe. |
| G6-9 | Native codec drops `signalGraph.mixers` on to_json (known-lossy round-trip for `dspchain_v2`) | LOW | Pre-existing native gap (json_codec.cpp:124–127,236–240 validates but never writes `mixers` back); `dspchain_v2` excluded from the round-trip corpus; tracked here for a future native gate (ORC-G6-07). |

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

## 10. Gate Record — two-reviewer gate, landed (2026-09-15)

### 10.1 P0 gate — oracle verdict (out-of-band, AMEND — landed)

| ID | Sev | Finding | Amendment landed |
|---|---|---|---|
| ORC-G6-01 | HIGH | Source-less `SHARED` target cannot configure (CMake 4.4.3 probe: "No SOURCES given to target: soundforge") — collides with "build config only" in §1.4 | §1.2/§1.3/§1.4 + D2 — 1-line anchor TU `host_stub.cpp` (generated by `file(GENERATE …)`), no engine logic |
| ORC-G6-02 | HIGH | `Threads::Threads` not visible in `src/host/` (imported targets are directory-scoped; `find_package` only exists in src/core\|graph\|audio) | D2 — `find_package(Threads REQUIRED)` in `src/host/CMakeLists.txt` |
| ORC-G6-03 | HIGH | Default artifact path is `build/src/host/libsoundforge.so`, not `build/libsoundforge.so` — every hardcoded consumer (D1/D3/D6/§6.4/DoD) would fail | D2 + §1.2 — `LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"` pin |
| ORC-G6-04 | HIGH | Migrate dict-equality unimplementable: both migrators inject fresh UUIDs + wall-clock ms timestamps | D4/P1 — normalized compare (mask UUID-v4 + RFC3339 fields, auditLog as ordered tuples, `engineVersion` strict) |
| ORC-G6-05 | HIGH | Silence assertion is against the wrong field: engine emits `truePeakLinear [0.0,0.0]`, `truePeakDb [null,null]` (not null linear) | D4/P2/DoD/§6.2 — silence → `truePeakLinear == 0.0` + `truePeakDb == null` |
| ORC-G6-06 | MED | Round-trip "semantic equality" fails on `signalgraph_v2`: to_json always emits `dspPresetRef:null`/`gainDb:0.0` | D4/P1 — canonical-form idempotence (`to_json(from_json(X))` pass1==pass2==pass3), no raw-bytes compare |
| ORC-G6-07 | MED | `dspchain_v2` round-trip is lossy: native codec drops `signalGraph.mixers` | P1 + residual G6-9 — excluded from round-trip corpus; scrub "on v2 fixtures" wording |
| ORC-G6-08 | MED | Migrate buffer sizing unspecified; naive `create_string_buffer(raw)` → `SF_E_NOMEM` (pretty ≈15.6× minified) | D3 — `max(8*len(raw), 1<<16)` or NOMEM-retry doubling; `*inout_len` excludes NUL, cap = buffer size |
| ORC-G6-09 | MED | Getter-lockstep ambiguity: `sf_project_get_engine_version` returns the fixture's stamp (`0.1.0-g1`), never `__version__` | P1 — compare to fixture's own `engineVersion`; only `sf_engine_version()` vs `__version__` |
| ORC-G6-10 | MED | Tolerance contradiction (`1e-3/1e-6` vs `1e-5 rel`) in D4 vs P2 | D4/P2 — per-surface: meter 1e-3; DC block ≤1e-6 abs (unit-test parity); sine block ≤1e-5 rel — `regression/engine_parity.py` single source |
| ORC-G6-11 | MED | Python CFUNCTYPE callbacks violate the header's lock-free/alloc-free advisory | §9.2 G6-8 residual — safe (start(0), no pacer, caller thread); future PACE gate must be GIL-safe |
| ORC-G6-12 | LOW | Version surface #2 edits only `SF_BUILD_VERSION`; adjacent comment history + `g5+build.123` example also stale | §4.2 surface #2 — bump history line (G5 precedent ORC-G5-06) |
| ORC-G6-13 | LOW | Wrapped-count bookkeeping wrong ("6 getters" → 4) + `≥39` nm gate is weak (65 export) | D3 table + D1/§6.4/DoD — explicit wrapped-name-list gate |
| ORC-G6-14 | INFO | `char** out_json` free trap: `out.contents` raises AttributeError on `c_char_p`; free via `cast(out, c_void_p)` | D3 — documented in `_take_string` row |
| ORC-G6-15 | INFO | `-R Version` 4/4 includes `Migration.V0ToV1SchemaVersionOne` (substring) | No action — accepted, matches G5 |
| ORC-G6-16 | INFO | Validator-divergence escalation risk retired: decision parity holds on **all 9** fixtures (probe) | P1 — noted; 5-fixture corpus fine, full corpus also passes |
| ORC-G6-17 | INFO | Whole-archive probe: off → 0 `T sf_` exports; on → 65; bare-name form links clean (no duplicate-definition hazard); `<TARGET_FILE>`+push/pop form fails at generate time | D2 amendment text updated (drop duplicate-hazard implication) |

### 10.2 P0 gate — security reviewer verdict (out-of-band, AMEND — landed)

| ID | Sev | Finding | Amendment landed |
|---|---|---|---|
| SEC-G6-01 | MED | Env-controlled dlopen acceptable for dev/test harness (no install, no entry points) but must harden: reject non-absolute `SOUNDFORGE_LIB_PATH`, realpath-canonicalize + isfile-check | D3 loader — absolute-path enforcement + runtime error naming both candidate paths; loader documented test-only trust boundary |
| SEC-G6-02 | MED | Pin `RTLD_LOCAL`; whole-archive exports the full mangled C++ surface — acceptable under LOCAL; never RTLD_GLOBAL | D3 loader + D6 rationale — explicit `ctypes.CDLL(path, mode=RTLD_LOCAL)`; future-ASan note kept |
| SEC-G6-03 | MED | Length-parameter discipline: byte length, never char count — char-count len on non-ASCII JSON = OOB read in the .so | D3 — `_as_bytes()` helper for all JSON-entry calls + P1 non-ASCII (`café`) parity case |
| SEC-G6-04 | MED | `sf_free_string` single-owner rule for string-outs; error strings valid only until next error-set (same-thread) | D3 — `_take_string()` sole free path; documented in engine docstring |
| SEC-G6-05 | MED | io-callback contract: copy-within-call, try/except → `SF_E_IO` (never 0==SF_OK), channels==2 tripwire, CFUNCTYPE kept alive on wrapper | D4 mechanics + P2 acceptance — binding REQUIRED items |
| SEC-G6-06 | LOW | No `__del__`/finalizers for auto-free; AudioEngine holds strong refs; destruction via context managers only | D3 acceptance + P2 — documented |
| SEC-G6-07 | LOW | Parser differentials: feed identical bytes to both sides (single serialization); fixtures as raw bytes | D4 mechanics — byte-identity rule |
| SEC-G6-08 | INFO | conftest/sys.path isolation confirmed: file-relative repo_root, session-scoped lazy fixture, no import-time engine load | D3 acceptance — conftest contract |
| SEC-G6-09 | LOW | G6-3 residual upgrade: security-review trigger before pip packaging/console scripts (loader → installed consumption) | §9.2 G6-3 row annotated |
| SEC-G6-10 | PASS | float64 reference vs float32 engine — no security issue (1e-5/1e-3 tolerances, fixtures away from denormals) | — |

### 10.3 Q&A table — two-reviewer gate

| Question | Oracle verdict | Security verdict | Landed in |
|---|---|---|---|
| Q1 binding = ctypes | APPROVE — surface already `extern "C"`; cffi/pybind11 add build coupling for zero coverage | Endorse — with SEC-G6-03/05 as binding constraints | D1 |
| Q2 shared-lib guard + path contract | APPROVE **WITH AMENDMENT** — ORC-G6-01/02/03 mandatory fixes; no-install-target fine | Endorse — with SEC-G6-01/02 hardening REQUIRED | D2/D3 |
| Q3 UBSan parity posture | APPROVE — verified UBSan-only (dlopen-safe, no LD_PRELOAD); TSan exclusion correct; P1 must confirm env-override actually loads build-asan | Endorse — dlopen-safe analysis verified | D6 |
| Q4 reference scope (DC-only meter) | APPROVE — honest and exact; correct silence field (ORC-G6-05) + pin tolerance metric (ORC-G6-10) | Endorse — right boundary | D5/D4 |
| Q5 app stamp KEEP | APPROVE — KEEP `0.1.0-g0` (G6-5); no 8th surface | Endorse — version hygiene only | §4.2 |

---

*End of PLAN_G6.md — amended post two-reviewer gate (ORC-G6-01..17 + SEC-G6-01..10 landed, 2026-09-15); P1-ready.*