# SoundForge / PA Studio — G0 Implementation Plan

**Gate:** G0 — Product Shell, Project Schema, Engine Versioning, Diagnostics  
**Spec refs:** §7 Architecture, §9 Canonical Project Data Model, §23 Diagnostics, §31 Repository Layout, Appendix A File Map, Appendix B Exit Criteria  
**Scope discipline:** G0 only. No G1–G9 behavior (DSP execution, array optimization, acoustics simulation, signal graph evaluation, measurement capture, training runtime). G0 is *skeleton that compiles, launches, creates/opens/saves/migrates a project, reports versions, and logs crash-safely.*

> How to use this plan: Work phase-by-phase in §7 order. Each file has a bounded G0 contract — do not expand into G1+. Checklists are gated by Appendix B exit criteria.

---

## Table of Contents

1. [G0 Scope & Non-Goals](#1-g0-scope--non-goals)
2. [File / Module Breakdown (§7, §31, Appendix A) — G0-Bounded](#2-file--module-breakdown-7-31-appendix-a--g0-bounded)
3. [Canonical Project Data Model (§9) — Structs, Schema & Persistence](#3-canonical-project-data-model-9--structs-schema--persistence)
4. [Native Bridge API — Kotlin ↔ C++ Core](#4-native-bridge-api--kotlin--c-core)
5. [Versioning & Audit Log Design](#5-versioning--audit-log-design)
6. [Diagnostics & Logging Requirements (§23) — G0](#6-diagnostics--logging-requirements-23--g0)
7. [Test Plan — Appendix B Exit Criteria for G0](#7-test-plan--appendix-b-exit-criteria-for-g0)
8. [Build Order / Phase Steps](#8-build-order--phase-steps)
9. [Definition of Done (Gate G0)](#9-definition-of-done-gate-g0)

---

## 1. G0 Scope & Non-Goals

### G0 MUST deliver

| Pillar | Spec | G0 Deliverable |
|---|---|---|
| **Product shell** | §7, §31, Appx A | Android app compiles (Gradle + CMake), launches `MainActivity` → `HomeScreen` → `ProjectListScreen`, empty placeholder screens for future gates, navigation host, theme, app icon stub |
| **Project schema** | §9 | Canonical JSON schema + C++ structs for all top-level collections, `schemaVersion` + `engineVersion` on every project, JSON serialize/deserialize, file-based persistence (create/open/save) |
| **Engine versioning** | §23, gate G0 | `sf_version.h` returns SemVer from build stamp, schema version constant, compatibility check, migration stub (v0→v1 path exercised by tests) |
| **Diagnostics** | §23 | Crash-safe ring logger, `last_error_message()`, startup capability probe *outline* (not full probe), project health check (schema validate), log sink visible via `adb logcat` + file |

### G0 MUST NOT deliver (defer, leave stubs)

- G1 Scene/venue editing logic, geometry kernels — **stub** `SceneEditorScreen.kt` with no graph
- G2 Signal graph evaluation, routing, mixer engine — **stub** `SignalEditorScreen.kt`, `MixerScreen.kt`, `native/src/graph/*` empty with CMake stub
- G3 DSP chain execution — **stub** `DspChainScreen.kt`, `native/src/dsp/*` empty
- G4 Array configuration & optimization — **stub** `PredictionScreen.kt`, `native/src/arrays/*`
- G5 Acoustics/prediction simulation — `native/src/acoustics/*`, `native/src/render/*` empty
- G6 Measurement import/capture — `MeasurementScreen.kt` stub, `native/src/measurement/*` empty
- G7 Power graph — `native/src/power/*` empty
- G8 Training scenarios — `TrainingScreen.kt` stub, `native/src/optimization/*` empty
- G9 Reports/export — `ReportsScreen.kt` stub
- Python reference/regression parity — `python/*` stubs only (no NumPy engine yet)
- Inventory, USB, real-time audio I/O — `app/platform/audio|usb/*` interfaces only

> **Gating rule:** If a file is listed as *stub* below, its G0 implementation is ≤20 LOC: compiles, is reachable from `include/`, and contains a `// G0: stub — G1+ implements` comment. No logic.

---

## 2. File / Module Breakdown (§7, §31, Appendix A) — G0-Bounded

Canonical root: `/root/project/soundforge/` . All paths below are relative. Items marked **NEW** must be created, **EDIT** must be fleshed from current stub, **STUB** is intentionally empty directory + `CMakeLists.txt` or `*.kt` placeholder that compiles.

### 2.1 Top-level & Build

```
soundforge/
├── app/
│   ├── build.gradle.kts              # EDIT — G0: apply android+kotlin, define namespace id.soundforge.pastudio, minSdk 26, targetSdk 34, NDK/CMake linkage
│   ├── CMakeLists.txt                # EDIT — G0: delegates to ../native/CMakeLists.txt via add_subdirectory or ExternalNativeBuild; currently stub
│   └── settings.gradle.kts           # NEW  — G0: include(":app")
├── native/
│   ├── CMakeLists.txt                # EDIT — G0: project(SoundForgeNative CXX 20), add_subdirectory(src/core), options for tests
│   └── data/
│       ├── schemas/project_schema.json  # NEW — G0: canonical JSON Schema (see §3)
│       ├── migrations/                  # NEW — G0: empty dir + README + example migration manifest (json)
│       └── example_devices/             # STUB — G0: empty, keep .gitkeep
├── python/
│   ├── pyproject.toml                # NEW — G0: package soundforge_py, Python 3.10+, deps: jsonschema (for schema tests)
│   └── README.md                     # NEW — G0: notes that reference engine is G0-stub
├── tests/
│   ├── CMakeLists.txt                # NEW — G0: native unit test target (GoogleTest or Catch2, header-only vendored or FetchContent)
│   └── ...                           # see 2.5
└── docs/
    └── PLAN_G0.md                    # THIS FILE
```

### 2.2 `app/ui/*` — Kotlin / Compose Shell

Per §7: UI is pure presentation; all state mutation goes through native bridge (or ViewModel→bridge). G0: navigation + empty screens + project list backed by storage interface.

```
app/ui/
├── SoundForgeApplication.kt          # EDIT — G0: Application class; init diagnostics (SFDiagnostic.init), set global exception handler stub, no audio init yet
├── MainActivity.kt                   # EDIT — G0: ComponentActivity, setContent { SoundForgeTheme { NavHost } }, edge-to-edge, no permissions request yet
├── navigation/
│   ├── NavGraph.kt                   # NEW — G0: NavHost with routes: home, projectList, scene, signal, mixer, dsp, prediction, measurement, training, reports
│   └── Routes.kt                     # NEW — G0: sealed class Route(val path: String) — one object per screen
├── theme/
│   ├── Theme.kt                      # NEW — G0: Material3 theme stub, light/dark, typography
│   └── Color.kt                      # NEW — G0: palette placeholder
├── home/
│   └── HomeScreen.kt                 # EDIT — G0: shows engineVersion/schemaVersion, "Create Project" / "Open Project" buttons, recent projects list (max 5), diagnostics status chip
├── project/
│   ├── ProjectListScreen.kt          # EDIT — G0: lists *.sfproj files via StorageRepository, create/open/delete (delete is soft stub), navigates to Home on open
│   ├── ProjectViewModel.kt           # NEW — G0: ViewModel holding ProjectHandle? + UiState (Loading/Ready/Error), calls NativeBridge.projectCreate/Open/Save, exposes StateFlow
│   └── NewProjectDialog.kt           # NEW — G0: simple AlertDialog: name + venue preset dropdown (stub presets)
├── scene/SceneEditorScreen.kt        # EDIT — G0: placeholder scaffold: TopAppBar + "Scene editor — available in G1" + project name header; no editing
├── signal/SignalEditorScreen.kt      # EDIT — G0: placeholder
├── mixer/MixerScreen.kt              # EDIT — G0: placeholder
├── dsp/DspChainScreen.kt             # EDIT — G0: placeholder
├── prediction/PredictionScreen.kt    # EDIT — G0: placeholder
├── measurement/MeasurementScreen.kt  # EDIT — G0: placeholder
├── training/TrainingScreen.kt        # EDIT — G0: placeholder
└── reports/ReportsScreen.kt          # EDIT — G0: placeholder
```

**G0 contracts per screen:**
- Must compile, be reachable from `NavGraph`, and display a `Text("G0 placeholder")` + `projectName` if a project is open.
- No ViewModel beyond `ProjectViewModel` in G0. Other screens take `projectId: String?` nav arg and render empty state.

### 2.3 `app/platform/*` — Android Platform Abstractions

Per §7: platform layer isolates Android I/O so C++ core stays portable. G0: interfaces + no-op / file-based implementations; no real audio/USB.

```
app/platform/
├── diagnostics/
│   ├── SfLogger.kt                   # NEW — G0: wrapper around native sf_diagnostics.h log_message + Android Logcat; ring buffer size 64KB; init() sets sink
│   └── CrashHandler.kt               # NEW — G0: Thread.setDefaultUncaughtExceptionHandler → flush logger → write tombstone to filesDir/crash.log
├── storage/
│   ├── StorageRepository.kt          # NEW — G0: interface { suspend fun listProjects(): List<ProjectMeta>; suspend fun readProject(path): Result<ByteArray>; suspend fun writeProject(path, bytes) }
│   ├── FileStorageRepository.kt      # NEW — G0: implements via java.io.File under context.filesDir / "projects"; no SAF in G0 (internal storage only)
│   └── ProjectFile.kt                # NEW — G0: data class ProjectFile(path, name, modifiedAt, schemaVersion, engineVersion); codec for JSON bytes ↔ handle
├── permissions/
│   └── PermissionManager.kt          # NEW — G0: stub interface; G0 returns granted=false for all, with rationale strings pre-authored; no actual request
├── audio/
│   └── AudioCapabilityProbe.kt       # NEW — G0: OUTLINE only — interface CapabilityReport { sampleRates: List<Int>, lowLatency: Boolean }; impl returns hardcoded [44100,48000] without probing hardware yet
└── usb/
    └── UsbManagerStub.kt             # NEW — G0: empty object with isSupported=false; satisfies future dependency without permission code
```

### 2.4 `native/*` — C++ Core (Portable, NDK-buildable)

Per §7: `native/include/soundforge` is public ABI; `native/src/*` is implementation. G0 only `src/core` has logic; all other `src/*` are CMake stubs.

```
native/
├── include/soundforge/
│   ├── sf_types.h                    # EDIT — G0: expand (see §3): UUID, Timestamp, Result<T>, StringView helpers; AudioBuffer stays but G0 does not use
│   ├── sf_version.h                  # EDIT — G0: add SF_SCHEMA_VERSION constant, sf_engine_version(), sf_schema_version(), sf_is_compatible(schemaVer)
│   ├── sf_project.h                  # EDIT — G0: replace stub with full C ABI (see §4) + C++ handle struct SfProject; opaque handle typedef sf_project_t
│   ├── sf_diagnostics.h              # EDIT — G0: expand to C ABI: sf_log(), sf_last_error(), sf_set_log_sink(), sf_flush_logs(), sf_health_check()
│   ├── sf_schema.h                   # NEW — G0: JSON schema validation entry: bool sf_validate_project_json(const char* json, char* errBuf, size_t errCap)
│   └── sf_migration.h                # NEW — G0: migration API: int sf_migrate_json(char* jsonInOut, size_t cap, int fromVer, int toVer)
├── src/
│   ├── core/
│   │   ├── CMakeLists.txt            # EDIT — G0: add all core cpp files, link nlohmann/json (vendored single header) or rapidjson
│   │   ├── project.cpp               # EDIT — G0: implement SfProject handle, create/open/save/validate, JSON codec
│   │   ├── version.cpp               # EDIT — G0: return build-stamped version (from generated version_gen.h) + schema compat table
│   │   ├── version_gen.h.in          # NEW — G0: CMake configure_file template: #define SF_VERSION "0.1.0-g0"
│   │   ├── diagnostics.cpp           # EDIT — G0: ring buffer (64KB), thread-safe, crash-safe flush, last_error TLS
│   │   ├── schema.cpp                # NEW — G0: validates against project_schema.json (embedded via xxd/incbin or file path in tests)
│   │   ├── migration.cpp             # NEW — G0: stub migrator: if fromVer==0 && toVer==1 inject defaults; otherwise error
│   │   ├── json_codec.cpp            # NEW — G0: nlohmann::json ↔ SfProject serialization (all §9 fields, many empty arrays in G0)
│   │   └── uuid.cpp                  # NEW — G0: UUID v4 generator (platform-agnostic, uses <random>)
│   ├── graph/CMakeLists.txt          # NEW — G0: stub library sfgraph (empty) — satisfies Appendix A without logic
│   ├── dsp/CMakeLists.txt            # NEW — G0: stub sfdsp
│   ├── audio/CMakeLists.txt          # NEW — G0: stub sfaudio
│   ├── acoustics/CMakeLists.txt      # NEW — G0: stub sfacoustics
│   ├── arrays/CMakeLists.txt         # NEW — G0: stub sfarrays
│   ├── power/CMakeLists.txt          # NEW — G0: stub sfpower
│   ├── render/CMakeLists.txt         # NEW — G0: stub sfrender
│   ├── measurement/CMakeLists.txt    # NEW — G0: stub sfmeasurement
│   └── optimization/CMakeLists.txt   # NEW — G0: stub sfoptimization
├── data/
│   ├── schemas/project_schema.json   # NEW — G0: authoritative schema (Draft 2020-12)
│   ├── migrations/manifest.json      # NEW — G0: { "latest": 1, "migrations": [{"from":0,"to":1,"description":"G0 init"}] }
│   └── example_devices/.gitkeep      # STUB
└── CMakeLists.txt                    # top-level (see 2.1)
```

**Vendor note G0:** Add `native/third_party/nlohmann/json.hpp` (single header v3.11) — permissive MIT, no submodule network fetch required for CI. Alternatively `rapidjson` if preferred. Document choice in `native/README.md` (NEW, G0: 10-line note).

### 2.5 `python/*` — Reference & Tooling (G0 stubs)

Per §31: Python is reference/model & import scripts; not required for G0 gate but layout must exist.

```
python/
├── pyproject.toml                    # NEW — G0: [project] name="soundforge-py"
├── soundforge_py/
│   ├── __init__.py                   # NEW — G0: exposes __version__ = "0.1.0-g0", SCHEMA_VERSION = 1
│   ├── schema.py                     # NEW — G0: loads native/data/schemas/project_schema.json, validates via jsonschema; function validate_project(path)
│   └── migrate.py                    # NEW — G0: stub migrate_json(data, from_ver, to_ver) — mirrors C++ stub
├── data_import/__init__.py           # NEW — G0: empty
├── reference/__init__.py             # NEW — G0: empty
└── regression/__init__.py            # NEW — G0: empty
```

### 2.6 `tests/*` — Test Layout (Appendix B)

```
tests/
├── CMakeLists.txt                    # NEW — G0: add_subdirectory(unit) etc. if using C++ tests
├── fixtures/
│   ├── project_minimal_v0.json       # NEW — G0: smallest valid v0 project (pre-migration)
│   ├── project_minimal_v1.json       # NEW — G0: smallest valid v1 project (post-migration)
│   ├── project_full_empty_v1.json    # NEW — G0: all arrays empty but all required keys present, UUIDs filled
│   └── project_corrupt.json          # NEW — G0: missing required fields, bad UUID, wrong types — for negative tests
├── golden/
│   └── schema_golden_v1.json         # NEW — G0: snapshot of schema to detect accidental drift
├── unit/
│   ├── CMakeLists.txt                # NEW — G0: builds sf_tests
│   ├── test_version.cpp              # NEW — G0: version string/semver, compatibility
│   ├── test_project_create.cpp       # NEW — G0: create → fields, UUIDs, timestamps
│   ├── test_project_serialize.cpp    # NEW — G0: round-trip JSON
│   ├── test_schema_validate.cpp      # NEW — G0: validate fixtures
│   ├── test_migration.cpp            # NEW — G0: v0→v1 injects defaults
│   └── test_diagnostics.cpp          # NEW — G0: log, last_error, ring buffer flush
├── integration/
│   ├── test_project_io.cpp           # NEW — G0: file create/open/save/reopen (tmpdir), health check
│   └── test_kotlin_bridge_jni.cpp    # NEW — G0: exercises C ABI via JNI-less direct calls (handle lifecycle)
└── python_tests/
    └── test_schema_py.py             # NEW — G0: pytest validates same fixtures via python/soundforge_py/schema.py
```

> **Bound check:** If a path above is not listed, do not create it in G0. Future gates will populate `src/acoustics/*`, `src/dsp/*`, etc.

---

## 3. Canonical Project Data Model (§9) — Structs, Schema & Persistence

### 3.1 Top-level Project object (§9 verbatim fields)

G0 persists **one JSON file per project** (`<name>.sfproj`). Binary sidecars (audio assets) are *referenced* but not embedded in G0 — `audioAssets[].localPath` may point to missing files; health check warns, does not fail.

```jsonc
// <name>.sfproj — canonical shape, schemaVersion=1, G0 example (pretty-printed)
{
  "schemaVersion": 1,
  "engineVersion": "0.1.0-g0",
  "project": {
    "id": "550e8400-e29b-41d4-a716-446655440000",
    "name": "Demo PA",
    "createdAt": "2026-09-11T00:00:00Z",
    "modifiedAt": "2026-09-11T00:00:00Z",
    "author": "",
    "notes": ""
  },
  "venue": { "id": "…", "name": "Untitled Venue", "dimensions": null, "…": "G1+" },
  "scene": { "id": "…", "name": "Default Scene", "venueRef": "…" },
  "audienceReceivers": [],
  "equipment": [],
  "signalGraph": { "nodes": [], "edges": [] },
  "powerGraph": { "nodes": [], "edges": [] },
  "audioAssets": [],
  "dspPresets": [],
  "arrayConfigurations": [],
  "measurements": [],
  "simulationRuns": [],
  "trainingScenarios": [],
  "inventoryRefs": [],
  "reports": [],
  "auditLog": [
    { "ts": "2026-09-11T00:00:00Z", "actor": "system", "action": "project.create", "objectId": "…", "detail": "schemaVersion 1" }
  ]
}
```

All 17 top-level keys **must be present** in G0 (even if empty/placeholder). This satisfies §9 "project is a closed document" and lets later gates append without migration.

### 3.2 Key structs / classes

#### C++ Core (`native/include/soundforge/sf_project.h` C++ view; C ABI is opaque handle)

```cpp
// sf_types.h additions — G0
namespace soundforge {
  using Uuid = std::string;          // canonical "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" lower-case
  using IsoTimestamp = std::string;  // RFC3339 UTC, e.g. 2026-09-11T00:00:00Z
  Uuid        uuid_generate();        // v4
  IsoTimestamp now_iso8601();         // UTC

  enum class SfResult : int32_t { Ok=0, InvalidArg, NotFound, SchemaError, VersionMismatch, IoError, NoMem };

  struct AuditEntry {
    IsoTimestamp ts;
    std::string  actor;      // "system" | "user:<name>" in G0
    std::string  action;     // "project.create" | "project.open" | "project.save" | "project.migrate"
    Uuid         objectId;   // project id or affected object id
    std::string  detail;     // free-form, truncated to 512 chars
  };

  // Forward — full definitions in src/core/json_codec.cpp translation unit, exposed as plain structs for tests
  struct ProjectMeta { Uuid id; std::string name; IsoTimestamp createdAt, modifiedAt; std::string author, notes; };
  struct VenueStub   { Uuid id; std::string name; };          // G0: only id+name, rest is json::object placeholder
  struct SceneStub   { Uuid id; std::string name; Uuid venueRef; };

  // Generic object envelope for all collections — G0 stores as json::array of envelopes
  struct ObjectEnvelope {
    Uuid        id;
    std::string type;            // "audienceReceiver" | "equipment" | ...
    int         version = 1;
    IsoTimestamp createdAt, modifiedAt;
    std::string provenance;      // "created" | "imported" | "migrated:v0->v1"
    nlohmann::json data;         // type-specific payload — empty object {} in G0
  };

  struct SfProjectDoc {
    int         schemaVersion = 1;
    std::string engineVersion;   // e.g. "0.1.0-g0"
    ProjectMeta project;
    VenueStub   venue;
    SceneStub   scene;
    std::vector<ObjectEnvelope> audienceReceivers;
    std::vector<ObjectEnvelope> equipment;
    nlohmann::json signalGraph;  // {nodes:[], edges:[]} in G0
    nlohmann::json powerGraph;
    std::vector<ObjectEnvelope> audioAssets;
    std::vector<ObjectEnvelope> dspPresets;
    std::vector<ObjectEnvelope> arrayConfigurations;
    std::vector<ObjectEnvelope> measurements;
    std::vector<ObjectEnvelope> simulationRuns;
    std::vector<ObjectEnvelope> trainingScenarios;
    std::vector<ObjectEnvelope> inventoryRefs;
    std::vector<ObjectEnvelope> reports;
    std::vector<AuditEntry>     auditLog;
  };

  // Opaque handle owned by native heap — Kotlin holds jlong
  struct SfProject { SfProjectDoc doc; std::string lastError; };
  using sf_project_t = SfProject;
}
```

**Design notes:**
- `ObjectEnvelope` unifies the 12 collection types (§9) so schema/migration/audit logic is written once. G0 payload `data` is `{}`; later gates replace with typed structs without changing persistence envelope.
- `signalGraph`/`powerGraph` are `json` in G0 (empty graph). G1+ may promote to typed graph structs; migration will translate.
- No binary blob in G0. `audioAssets[].data.provenance.localPath` is a relative string; file existence is not required for health check `OK` (warning only).

#### Kotlin (app layer)

```kotlin
// app/platform/storage/ProjectFile.kt — G0
data class ProjectMetaKt(
  val id: String, val name: String,
  val createdAt: String, val modifiedAt: String,
  val schemaVersion: Int, val engineVersion: String
)
data class ProjectHandle(val nativePtr: Long) // jlong to sf_project_t*
```

Kotlin does **not** duplicate the full 17-field model in G0 — it treats project as opaque `ByteArray`/`String` JSON and delegates parse/validate to native. `ProjectViewModel` only surfaces `ProjectMetaKt` for list UI. Full typed Kotlin mirrors are G1+ (when UI needs field-level editing).

#### Python

```python
# python/soundforge_py/schema.py — G0 thin wrapper
SCHEMA_VERSION: int = 1
ENGINE_VERSION: str = "0.1.0-g0"
def validate_project(data: dict) -> list[str]: ...
```

### 3.3 JSON Schema (`native/data/schemas/project_schema.json`)

- Draft: `https://json-schema.org/draft/2020-12/schema`
- `$id`: `https://soundforge.pa/schemas/project_v1.json`
- `required`: all 17 top-level keys listed in §3.1
- `properties.schemaVersion`: `{ "const": 1 }` in G0 (later becomes `{ "minimum": 1 }`)
- `properties.engineVersion`: `{ "type":"string", "pattern":"^\\d+\\.\\d+\\.\\d+.*$" }`
- `properties.project`: required `id` (uuid format), `name` (minLength 1), `createdAt`/`modifiedAt` (date-time), `author`/`notes` strings
- Collections: `{ "type":"array", "items": { "$ref":"#/$defs/objectEnvelope" } }`
- `$defs.objectEnvelope`: requires `id` (uuid), `type` (enum of 12 collection type strings), `version`, `createdAt`, `modifiedAt`, `provenance`, `data` (object)
- `signalGraph`/`powerGraph`: `{ "type":"object", "required":["nodes","edges"], "properties": { "nodes":{"type":"array"}, "edges":{"type":"array"} } }`
- `auditLog`: array of `{ ts, actor, action, objectId, detail }` with `action` enum `["project.create","project.open","project.save","project.migrate"]` in G0
- `additionalProperties: false` at top level (closed document) — catches typos early

Embed strategy: In C++ tests, load from file path `native/data/schemas/project_schema.json` (relative to test binary via `__FILE__` or CMake `-D SCHEMA_PATH`). In Android at runtime G0 may skip file-based schema load and use compiled-in validation (structural checks) — full json-schema validation is test-only in G0 to avoid extra native deps. `sf_validate_project_json` implements structural checks (required keys + uuid format + schemaVersion) not full draft 2020-12.

### 3.4 JSON / Binary Persistence Approach

| Concern | G0 Decision |
|---|---|
| **Primary file** | Single UTF-8 JSON `.sfproj` (pretty-printed, 2-space indent, sorted keys for deterministic diffs). File name = sanitized `project.name` + `.sfproj`. |
| **Binary sidecars** | None in G0. `audioAssets` entries have `data.localPath` relative to `.sfproj` dir; if missing, health check returns `Warning` not `Error`. G1+ will add `*.sfasset` binary blobs. |
| **Atomic save** | Write to `*.sfproj.tmp` in same directory → `fsync` → `rename` over target (POSIX atomic on Android ext4/f2fs). Preserve `createdAt` on save. |
| **Encoding** | UTF-8, LF, no BOM. `nlohmann::json` with `dump(2)`. |
| **Size limit** | G0 health check warns if file > 5 MB (future: warn, not fail). |
| **Backup** | On migrate, write `*.sfproj.bak.v<from>` alongside original before overwrite. |
| **No SQLite/Realm** | Intentionally file-per-project in G0; DB is G5+ if needed. |

**Codec flow:**

```
Kotlin FileStorageRepository.writeProject(path, jsonBytes)
  → JNI sf_project_save_to_json(handle, outBuf)  [or direct jsonCodec serialize]
  → atomic write

Kotlin readProject(path) → bytes → sf_project_open_from_json(bytes) → handle
  → if schemaVersion < SCHEMA_VERSION → sf_migrate_json() → re-validate → handle
```

---

## 4. Native Bridge API — Kotlin ↔ C++ Core

### 4.1 Principles (G0)

- **Stable C ABI** — `extern "C"` functions with C types only; no STL across boundary, no exceptions across boundary. Kotlin calls via JNI `external fun` that forwards to C ABI.
- **Opaque handles** — Kotlin holds `jlong` (`int64`) which is `reinterpret_cast<sf_project_t*>`. Zero/null means no project.
- **Threading** — G0: all bridge calls on Kotlin `Dispatchers.IO`; native is **not** thread-safe per-handle (documented). No command queue yet — direct call. Queue is G2+ for real-time.
- **Error reporting** — every function returns `SfResult` (int). Human message via `sf_last_error(handle)` / `sf_last_error_global()`. Kotlin maps to `Result<T>`.
- **Lifecycle** — create → use → destroy. No ref-counting in G0.
- **No over-design** — no graph mutation API, no DSP API, no async callbacks in G0.

### 4.2 C ABI Header (`native/include/soundforge/sf_project.h` — G0 final)

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct sf_project_s sf_project_t;  // opaque
typedef int32_t sf_result_t;
#define SF_OK 0
#define SF_E_INVALID_ARG 1
#define SF_E_NOT_FOUND 2
#define SF_E_SCHEMA 3
#define SF_E_VERSION 4
#define SF_E_IO 5
#define SF_E_NOMEM 6

// Version
const char* sf_engine_version(void);   // static string, e.g. "0.1.0-g0"
int32_t     sf_schema_version(void);   // e.g. 1
int32_t     sf_is_compatible(int32_t schemaVersion); // 1 if can open/migrate, 0 otherwise

// Lifecycle
sf_project_t* sf_project_create(const char* name, const char* author); // author may be NULL
void          sf_project_destroy(sf_project_t* p);
sf_result_t   sf_project_clone(const sf_project_t* src, sf_project_t** out);

// JSON codec — caller owns buffers; native copies
sf_result_t sf_project_to_json(const sf_project_t* p, char** out_json, size_t* out_len); // malloc'd; caller sf_free_string()
sf_result_t sf_project_from_json(const char* json, size_t len, sf_project_t** out);
sf_result_t sf_project_save_to_path(const sf_project_t* p, const char* path); // atomic write
sf_result_t sf_project_open_from_path(const char* path, sf_project_t** out);  // validates + migrates if needed

// Accessors (G0 minimal)
const char* sf_project_get_name(const sf_project_t* p);
const char* sf_project_get_id(const sf_project_t* p);
int32_t     sf_project_get_schema_version(const sf_project_t* p);
const char* sf_project_get_engine_version(const sf_project_t* p);

// Health & validation
sf_result_t sf_validate_project_json(const char* json, size_t len, char* err_buf, size_t err_cap);
sf_result_t sf_project_health_check(const sf_project_t* p, char* report_buf, size_t report_cap); // 0=healthy, 1=warnings, 2=errors

// Migration
sf_result_t sf_migrate_json(char* json_inout, size_t* inout_len, size_t cap, int32_t from_ver, int32_t to_ver);

// Diagnostics
void        sf_free_string(char* s); // free string returned by to_json
const char* sf_last_error(const sf_project_t* p); // thread-local if p!=NULL else global
const char* sf_last_error_global(void);

// Logging (see sf_diagnostics.h)
typedef enum { SF_LOG_DEBUG=0, SF_LOG_INFO, SF_LOG_WARN, SF_LOG_ERROR } sf_log_level_t;
void sf_log(sf_log_level_t level, const char* tag, const char* msg);
void sf_set_log_sink(void (*sink)(sf_log_level_t, const char*, const char*)); // G0: null = Logcat/default
void sf_flush_logs(void);

#ifdef __cplusplus
}
#endif
```

**Notes:**
- `sf_project_save_to_path` / `open_from_path` are the only file I/O in C++ for G0 (used by integration tests). On Android, Kotlin does file I/O and uses `from_json/to_json` to keep storage permission logic in Kotlin.
- `sf_migrate_json` is in-place with `cap` to avoid allocation; returns `SF_E_NOMEM` if cap insufficient.
- JNI layer is a *thin* passthrough: `app/platform/bridge/NativeBridge.kt` (NEW G0) declares `external fun` per C ABI entry, loads `libsfcore.so` via `System.loadLibrary("sfcore")`. No extra logic in JNI — mapping `jstring` ↔ `const char*` only.

### 4.3 Kotlin JNI Facade (`app/platform/bridge/NativeBridge.kt` — NEW G0)

```kotlin
object NativeBridge {
  init { System.loadLibrary("sfcore") }
  external fun engineVersion(): String
  external fun schemaVersion(): Int
  external fun isCompatible(schemaVersion: Int): Boolean
  external fun projectCreate(name: String, author: String?): Long // returns handle (jlong), 0 on error
  external fun projectDestroy(handle: Long)
  external fun projectToJson(handle: Long): String // throws on error with lastError
  external fun projectFromJson(json: String): Long
  external fun validateJson(json: String): String? // null if ok, else error string
  external fun healthCheck(handle: Long): String // JSON report {status, warnings[], errors[]}
  external fun lastError(handle: Long): String
  external fun log(level: Int, tag: String, msg: String)
}
```

- Each `external fun` is `private` + a `suspend fun` wrapper in `ProjectViewModel` that calls on `Dispatchers.IO` and maps errors to `UiState.Error`.
- **Command queue — NOT in G0.** Document in code comment: `// G0: synchronous calls; G2 introduces SfCommandQueue for audio-thread safety`.

### 4.4 Object Handles & Lifecycle Diagram

```
[ Kotlin ]                          [ Native (C ABI) ]
ProjectViewModel.create("Demo")
  → NativeBridge.projectCreate()  → sf_project_create() → malloc SfProject { doc with UUIDs, timestamps, auditLog=[create] }
  ← jlong handle                  ← sf_project_t*
  ... use ...
  toJson()                        → sf_project_to_json() → malloc char*
  save via FileStorageRepository     (Kotlin writes bytes)
  onCleared()                     → sf_project_destroy() → free
```

- Handles are **not** reference-counted in G0. Exactly one Kotlin owner (`ProjectViewModel`). `sf_project_clone` exists for tests only.
- Null-handle guard: every C function checks `p==nullptr` → `SF_E_INVALID_ARG` + `set_last_error("null handle")`.
- Leak check: `tests/integration/test_kotlin_bridge_jni.cpp` asserts `sf_project_create`/`destroy` pairs via address sanitizer in CI.

### 4.5 Error Reporting Contract

- Native **never throws** across ABI. Internal C++ exceptions are caught at ABI boundary → `SF_E_*` + `last_error` string.
- `last_error` is **thread-local** (`thread_local std::string t_lastError`) plus per-handle `SfProject::lastError` for handle-specific errors. `sf_last_error(p)` prefers handle storage if `p!=nullptr`.
- Kotlin maps: `if (handle==0L) throw SoundForgeException(NativeBridge.lastError(0L))` → `UiState.Error(message)`.
- Error strings are English, stable, and logged at `SF_LOG_ERROR` automatically.

---

## 5. Versioning & Audit Log Design

### 5.1 Version Constants

```cpp
// native/include/soundforge/sf_version.h — G0
#define SF_ENGINE_VERSION_MAJOR 0
#define SF_ENGINE_VERSION_MINOR 1
#define SF_ENGINE_VERSION_PATCH 0
#define SF_ENGINE_VERSION_SUFFIX "-g0"   // prerelease tag
#define SF_SCHEMA_VERSION 1              // integer, monotonic

// CMake generates native/src/core/version_gen.h from version_gen.h.in
// so CI can override -DSF_BUILD_VERSION="0.1.0-g0+build.123"
```

- **engineVersion** (SemVer string) — identifies binary that *wrote* the file. Stored per-project (`doc.engineVersion`). Informational, not used for open.
- **schemaVersion** (int) — determines *compatibility*. Gate for open/migrate.

Compatibility table G0:

| file schemaVersion | engine 0.1.0-g0 can | action |
|---|---|---|
| 0 | open with migration | `sf_migrate_json` 0→1, append `auditLog` entry `project.migrate`, update `schemaVersion=1`, `engineVersion` to current |
| 1 | open natively | validate |
| >1 | reject | `SF_E_VERSION`, message `"unsupported schemaVersion X (max 1)"` |

### 5.2 Object Identity

- Every top-level object and every `ObjectEnvelope` has `id: UUID v4` (lower-case, hyphenated). Generated once at creation, **never changed** by migration or save.
- `project.id` is the document root UUID; file name is derived from `project.name` but `id` is canonical identity (allows rename without identity loss).
- Future gates (G1+) will add `parentId` / `refs` but G0 does not — envelope `data` is empty.

### 5.3 Audit Log — Schema & Semantics

Per §9: `auditLog[]` is append-only, ordered by `ts` ascending.

```cpp
struct AuditEntry {
  IsoTimestamp ts;      // UTC, ms precision optional, RFC3339
  std::string  actor;   // "system" | "user:<displayName>" — G0 always "system"
  std::string  action;  // enum G0: project.create | project.open | project.save | project.migrate
  Uuid         objectId;// project.id for project-level actions
  std::string  detail;  // e.g. "created schemaVersion 1 engine 0.1.0-g0"
  // G0 does NOT yet store: ip, deviceId, diff — those are G5+
};
```

**Rules G0:**
- On `sf_project_create`: push one entry `project.create` with `ts=now_iso8601()`, `detail="schemaVersion 1 engine X"`.
- On `sf_project_open_from_path` / `from_json` with migration: push `project.migrate` entry (`detail="migrated 0->1"`).
- On `sf_project_save_to_path` / `to_json` (called before save): caller (Kotlin) is responsible to push `project.save`? **Decision G0:** Native `sf_project_save_to_path` appends `project.save` automatically; Kotlin `to_json` path does **not** (Kotlin appends before `writeProject`). Document in header.
- No deletion, no truncation in G0. Future gate may cap at 10k entries (oldest dropped with warning).

### 5.4 Provenance & Type Envelope

Every element in the 12 collections carries:

```json
{
  "id": "uuid",
  "type": "equipment",          // closed enum, see schema $defs
  "version": 1,
  "createdAt": "2026-09-11T00:00:00Z",
  "modifiedAt": "2026-09-11T00:00:00Z",
  "provenance": "created",       // G0: "created" | "migrated:v0->v1"
  "data": {}
}
```

`version` is per-object schema version (distinct from document `schemaVersion`). G0 all `1`. `provenance` records how the object entered the document — useful for later import deduplication.

### 5.5 Migration Strategy

- **Location:** `native/src/core/migration.cpp` + `native/data/migrations/manifest.json`
- **Discovery:** At open, read `schemaVersion` from JSON without full parse (fast peek via `json::parse` + check field). If `< SF_SCHEMA_VERSION`, call `sf_migrate_json`.
- **G0 migrator 0→1:**
  ```cpp
  // pseudo
  if (from==0 && to==1) {
    json j = parse(json_inout);
    j["schemaVersion"] = 1;
    if (!j.contains("engineVersion")) j["engineVersion"] = SF_ENGINE_VERSION;
    for (auto& key : kRequiredTopLevelKeys) if (!j.contains(key)) j[key] = emptyFor(key);
    // inject auditLog entry
    // ensure every envelope has id/version/provenance
    *inout_len = j.dump(2).copyTo(json_inout, cap);
    return SF_OK;
  }
  ```
- **Idempotence:** Migrator must be idempotent — running 0→1 twice yields same output (check `schemaVersion` already 1 → no-op).
- **Backup:** `sf_project_open_from_path` before migration copies file to `<path>.bak.v0` (best-effort, logs warning if fails but continues).
- **No downgrade** in G0. `from > to` → `SF_E_VERSION`.
- **Testing:** `tests/fixtures/project_minimal_v0.json` → migrate → equals `project_minimal_v1.json` (structural, UUIDs ignored) — see §7.

---

## 6. Diagnostics & Logging Requirements (§23) — G0

### 6.1 Startup Capability Probe — Outline (G0)

G0 does **not** fully implement hardware probing (that is G1+ with Oboe/AAudio). G0 delivers the *scaffolding* so later gates plug in without refactor.

```kotlin
// app/platform/audio/AudioCapabilityProbe.kt — G0 outline
interface AudioCapabilityProbe {
  data class Report(
    val osVersion: String,
    val abi: String,                 // arm64-v8a etc.
    val lowLatencySupported: Boolean, // G0: false (stub)
    val sampleRates: List<Int>,      // G0: [44100,48000]
    val channelCounts: List<Int>,    // G0: [2]
    val timestamp: String            // ISO8601
  )
  suspend fun probe(): Report        // G0: returns hardcoded Report without touching audio HAL
  fun formatForLog(report: Report): String
}
```

Native side:

```cpp
// sf_diagnostics.h — G0
struct SfCapabilityReport { /* mirrors Kotlin Report, C struct */ };
sf_result_t sf_capability_probe(SfCapabilityReport* out); // G0: fills hardcoded values, returns SF_OK
```

**G0 behavior:**
- Called once from `SoundForgeApplication.onCreate` on `Dispatchers.IO`, result logged at `INFO` and stored in `SfLogger.lastProbeReport`.
- `HomeScreen` shows a chip: `Capability: stub (48000 Hz)` — makes probe visible for manual QA.
- Future (G1) will replace body with `AAudio`/`Oboe` queries; interface stays stable.

### 6.2 Crash-Safe Logging

**Requirements §23:** survive native crash, flush on `SIGSEGV`/`SIGABRT` (best-effort), never allocate on log hot-path if possible, visible via `adb logcat` and persisted file.

**Design G0:**

```
                     ┌─────────────────────┐
Kotlin SfLogger  →→  │ sf_log() C ABI      │  →  ring buffer (64 KB, lock-free SPSC or mutex)
Android Logcat   ←←  │ sink callback       │  ←  SfLogger registered sink
filesDir/logs/        │ flush on demand     │  →  sf_flush_logs() writes ring → filesDir/logs/sf.log
tombstone             │ crash handler       │  →  CrashHandler → flush → filesDir/crash.log
```

- **Native ring buffer:** `native/src/core/diagnostics.cpp`
  ```cpp
  constexpr size_t kRingCap = 64 * 1024;
  struct Ring { char buf[kRingCap]; size_t head, tail; std::mutex m; };
  enum class Level { Debug, Info, Warn, Error };
  void sf_log(Level lvl, const char* tag, const char* msg) {
    // format: "[ISO8601][LEVEL][tag] msg\n" truncated to 512 chars
    // lock, copy into ring, unlock, also forward to sink if set
  }
  ```
  - Crash-safe: `sf_flush_logs()` is async-signal-safe subset — uses `write(2)` on pre-opened fd if possible; in G0, best-effort `fopen` + `fwrite` is acceptable (document limitation).
  - No heap alloc inside `sf_log` beyond formatting on stack (char[512]).

- **Kotlin sink:** `SfLogger` registers `sf_set_log_sink { lvl, tag, msg -> Log.println(lvl, "SF/$tag", msg) }` on init. Also mirrors to `filesDir/logs/sf.log` via periodic `sf_flush_logs()` every 5s (WorkManager stub not needed — simple `Handler.postDelayed` in `Application`).

- **Last error:** `thread_local std::string t_lastError` + per-handle `lastError`. `sf_log(ERROR, ...)` also sets `t_lastError` if message contains error context (opt-in).

- **Log levels in G0:**
  - `DEBUG`: JSON codec steps, migration details
  - `INFO`: project create/open/save, capability probe, version
  - `WARN`: health check warnings, missing audioAssets files, backup failures
  - `ERROR`: schema failures, version mismatch, I/O errors

### 6.3 Project Health Checks

Called in two places: on open (blocking) and on demand via `sf_project_health_check` (for UI).

```cpp
sf_result_t sf_project_health_check(const sf_project_t* p, char* report_buf, size_t cap);
// Returns SF_OK (0=healthy), SF_E_SCHEMA (warnings present), or error
// report_buf JSON: { "status":"ok"|"warning"|"error", "warnings":["..."], "errors":["..."], "stats":{...} }
```

**Checks G0 (all cheap, <1ms for empty project):**

| Check | Ok | Warning | Error |
|---|---|---|---|
| Schema validation | passes | — | fails (missing required keys, bad UUID, wrong types) |
| schemaVersion | ==1 | — | >1 or <0 |
| File size | <5MB | 5–20MB | >20MB |
| UUID uniqueness | all unique | — | duplicate UUID |
| Timestamp parse | valid RFC3339 | — | unparseable |
| audioAssets localPath | exists or empty | file missing | — |
| auditLog ordered | sorted asc | unsorted (auto-sort on save) | — |

**UI surfacing G0:**
- `HomeScreen`/`ProjectListScreen` shows health chip (green/yellow/red) after open, derived from `healthCheck` JSON.
- Warnings do **not** block open; errors do (show dialog with `report_buf`).
- Diagnostics log: `sf_log(WARN, "health", report)` on warnings.

---

## 7. Test Plan — Appendix B Exit Criteria for G0

> **Gate G0 exit criteria (Appendix B):** *Project can be created, opened, saved, migrated; schema tests pass; app launches to shell.* No audio/DSP correctness required.

### 7.1 Unit Tests — Native (`tests/unit/*`)

| Test file | Cases | Asserts |
|---|---|---|
| `test_version.cpp` | engineVersion matches `SF_ENGINE_VERSION`, schemaVersion ==1, `sf_is_compatible(0)==1`, `sf_is_compatible(1)==1`, `sf_is_compatible(2)==0`, `sf_is_compatible(-1)==0` | String + int |
| `test_project_create.cpp` | create with name, create with null author, create with empty name → `SF_E_INVALID_ARG`, check `project.id` is UUID v4 format, `createdAt==modifiedAt`, `auditLog.size()==1` with `project.create` | Handle lifecycle, field regex |
| `test_project_serialize.cpp` | round-trip: create → to_json → from_json → compare docs (UUIDs stable), pretty-print is valid JSON, required keys present | JSON equality |
| `test_schema_validate.cpp` | fixtures: `project_minimal_v1.json` → `SF_OK`, `project_full_empty_v1.json` → `SF_OK`, `project_corrupt.json` (missing keys, bad UUID) → `SF_E_SCHEMA` with err_buf non-empty, golden schema drift check (hash of schema file) | `sf_validate_project_json` |
| `test_migration.cpp` | `project_minimal_v0.json` → `sf_migrate_json(0,1)` → validate passes, `schemaVersion==1`, auditLog has migrate entry, idempotent second migrate is no-op, `migrate 1→1` no-op, `migrate 1→0` → `SF_E_VERSION`, insufficient cap → `SF_E_NOMEM` | JSON diff |
| `test_diagnostics.cpp` | `sf_log` at each level → ring contains entry, `sf_last_error` after error, `sf_flush_logs` creates file, thread-local isolation (2 threads log different errors, each sees own) | Ring + TLS |

**Runner:** `ctest` via `tests/CMakeLists.txt`. Add `-DENABLE_SANITIZERS=ON` for ASan/UBSan in CI. Vendored `googletest` via `FetchContent` (or `catch2` single header) — declare in `tests/CMakeLists.txt`.

### 7.2 Integration Tests (`tests/integration/*`)

| Test | Steps | Pass |
|---|---|---|
| `test_project_io.cpp` — create/open/save/reopen | 1. `sf_project_create("IntTest")` 2. `sf_project_save_to_path(tmp/sf_g0_test.sfproj)` 3. assert file exists & size>0 4. `sf_project_open_from_path` 5. compare `project.id` equals original 6. modify `project.name` via JSON edit → save → reopen → name persisted 7. `sf_project_health_check` → status ok | File I/O + atomic rename verified; no handle leaks (valgrind/ASan) |
| `test_project_io` — corrupt handling | Write `project_corrupt.json` to tmp path → `sf_project_open_from_path` → returns `SF_E_SCHEMA`, `sf_last_error_global` mentions missing keys | Error path |
| `test_project_io` — migration via file | Write `project_minimal_v0.json` to tmp → open → check `schemaVersion==1` and `.bak.v0` exists | Migration + backup |
| `test_kotlin_bridge_jni.cpp` | Direct C ABI lifecycle stress: 100× create/destroy, `to_json`/`from_json` with various strings, concurrent 4-thread create/destroy (checks thread-local last_error) — no JNI env needed | Stability |

**Temp dir:** Use `std::filesystem::temp_directory_path() / "sf_g0_tests"` created per run, cleaned on success.

### 7.3 Python Tests (`tests/python_tests/test_schema_py.py`)

```python
def test_validate_minimal_v1(): assert validate_project(load("project_minimal_v1.json")) == []
def test_validate_corrupt_fails(): assert len(validate_project(load("project_corrupt.json"))) > 0
def test_migrate_v0_to_v1(): data=load("project_minimal_v0.json"); migrate_json(data,0,1); assert validate_project(data)==[]
def test_schema_version_const(): assert SCHEMA_VERSION == 1
```

Run via `pytest python/tests` or `pytest tests/python_tests`. Shares fixtures with native tests (symlink or copy).

### 7.4 Android Instrumented Tests (G0 minimal)

```
app/src/androidTest/java/id/soundforge/pastudio/
├── ProjectStorageTest.kt   # NEW — G0: FileStorageRepositoryTest — create project via NativeBridge, write via repo, list, read, validate
└── NavigationSmokeTest.kt  # NEW — G0: launch MainActivity, assert HomeScreen displayed, tap "Projects" navigates, create dialog appears
```

- Use `androidx.test` + `composeTestRule`. No emulator audio needed.
- `ProjectStorageTest` runs on `Dispatchers.IO`, uses `ApplicationProvider.getApplicationContext().filesDir` isolated via `@Before` clear.

### 7.5 Manual QA Checklist (gate sign-off)

- [ ] `./gradlew :app:assembleDebug` succeeds (NDK r26, CMake 3.22)
- [ ] `adb install` + launch → HomeScreen shows `Engine 0.1.0-g0 / Schema 1` and no crash
- [ ] Create project "G0 Smoke" → appears in ProjectList → tap → Home shows project name
- [ ] Kill app → relaunch → project still listed
- [ ] `adb logcat -s SF` shows `INFO project.create` and capability probe
- [ ] Pull `filesDir/projects/G0_Smoke.sfproj` → `python -m soundforge_py.schema` validates
- [ ] Corrupt the file (remove `schemaVersion`) → open → error dialog with health report

---

## 8. Build Order / Phase Steps

Implement **strictly in order** — each phase is a vertical slice that keeps the tree green. Do not start phase N+1 until phase N compiles and its tests pass.

### Phase 0 — Scaffolding & Toolchain (½ day)

- [ ] 0.1 Ensure `native/third_party/nlohmann/json.hpp` vendored, add to `.gitignore` exceptions
- [ ] 0.2 Create/verify `native/CMakeLists.txt`, `native/src/core/CMakeLists.txt` compile stub (`project.cpp` currently returns null — keep compiling)
- [ ] 0.3 Create `app/settings.gradle.kts`, edit `app/build.gradle.kts` to apply `com.android.application` + `kotlin-android`, set `namespace`, `compileSdk 34`, `minSdk 26`, `ndkVersion`, `externalNativeBuild.cmake.path`
- [ ] 0.4 Verify `./gradlew :app:assembleDebug` **and** `cmake --build native/build` both succeed with stubs — gate to proceed

### Phase 1 — Versioning & Diagnostics Skeleton (1 day)

- [ ] 1.1 Implement `native/src/core/version.cpp` + `version_gen.h.in` + `sf_version.h` constants; test `test_version.cpp` passes
- [ ] 1.2 Implement `native/src/core/diagnostics.cpp` ring buffer + `sf_log`/`sf_last_error`/`sf_flush_logs`; test `test_diagnostics.cpp` passes
- [ ] 1.3 Implement Kotlin `SfLogger.kt` + `CrashHandler.kt` + register sink in `SoundForgeApplication.kt`; manually verify `adb logcat -s SF` shows `INFO` on launch

### Phase 2 — Project Data Model & Schema (1–1.5 days)

- [ ] 2.1 Author `native/data/schemas/project_schema.json` (closed, all 17 keys, `$defs/objectEnvelope`)
- [ ] 2.2 Implement `native/include/soundforge/sf_types.h` expansions (`Uuid`, `Timestamp`, `AuditEntry`, `SfProjectDoc`, `ObjectEnvelope`)
- [ ] 2.3 Implement `native/src/core/uuid.cpp` + `now_iso8601()` helper
- [ ] 2.4 Create fixtures `tests/fixtures/*.json` and golden `tests/golden/schema_golden_v1.json`; implement `python/soundforge_py/schema.py` + `pyproject.toml`; run `pytest tests/python_tests` — must pass before native schema
- [ ] 2.5 Implement `native/src/core/schema.cpp` (`sf_validate_project_json`) — structural checks only; test `test_schema_validate.cpp` passes

### Phase 3 — JSON Codec & Persistence (1.5 days)

- [ ] 3.1 Implement `native/src/core/json_codec.cpp` (serialize/deserialize `SfProjectDoc` ↔ `nlohmann::json`), handling all 17 keys, empty arrays default, UUID/timestamp generation
- [ ] 3.2 Implement `native/src/core/project.cpp` full handle lifecycle (`create`, `destroy`, `clone`, `to_json`, `from_json`, `save_to_path`, `open_from_path`) with atomic write + auditLog append
- [ ] 3.3 Tests: `test_project_create.cpp` + `test_project_serialize.cpp` + integration `test_project_io.cpp` (create/open/save/reopen) — all green

### Phase 4 — Migration (½ day)

- [ ] 4.1 Implement `native/src/core/migration.cpp` (`sf_migrate_json` 0→1, idempotent, backup)
- [ ] 4.2 Add `native/data/migrations/manifest.json`
- [ ] 4.3 Tests: `test_migration.cpp` + integration migration via file — green, fixtures compare

### Phase 5 — Native Bridge & Kotlin Facade (1 day)

- [ ] 5.1 Finalize C ABI in `native/include/soundforge/sf_project.h` + `sf_diagnostics.h` (add `sf_free_string`, `sf_last_error`, `sf_set_log_sink`)
- [ ] 5.2 Implement JNI bridge `app/platform/bridge/NativeBridge.kt` + `app/src/main/cpp/jni_bridge.cpp` (NEW — JNI passthrough, `JNI_OnLoad`, `jstring` ↔ `char*` conversions, handle `jlong` mapping)
- [ ] 5.3 Implement `app/platform/storage/StorageRepository.kt` + `FileStorageRepository.kt` + `ProjectFile.kt`
- [ ] 5.4 Implement `app/platform/audio/AudioCapabilityProbe.kt` stub + call from `SoundForgeApplication`
- [ ] 5.5 Test: `test_kotlin_bridge_jni.cpp` (C ABI stress) + Android `ProjectStorageTest.kt` instrumented

### Phase 6 — Product Shell UI (1 day)

- [ ] 6.1 Implement `app/ui/navigation/Routes.kt` + `NavGraph.kt` + `app/ui/theme/*`
- [ ] 6.2 Edit `MainActivity.kt` to host `NavHost`, edit `SoundForgeApplication.kt` to init diagnostics
- [ ] 6.3 Implement `ProjectViewModel.kt` + `NewProjectDialog.kt`
- [ ] 6.4 Edit `HomeScreen.kt` (engine/schema chip, recent projects) + `ProjectListScreen.kt` (list, create, open)
- [ ] 6.5 Leave `SceneEditorScreen`, `SignalEditorScreen`, `MixerScreen`, `DspChainScreen`, `PredictionScreen`, `MeasurementScreen`, `TrainingScreen`, `ReportsScreen` as placeholders but verify navigation to each does not crash
- [ ] 6.6 Add `app/platform/permissions/PermissionManager.kt` stub + `app/platform/usb/UsbManagerStub.kt`

### Phase 7 — Stub Modules & Polish (½ day)

- [ ] 7.1 Create stub `CMakeLists.txt` for each `native/src/{graph,dsp,audio,acoustics,arrays,power,render,measurement,optimization}` so top-level `native/CMakeLists.txt` can `add_subdirectory` without error
- [ ] 7.2 Add `native/README.md`, `python/README.md`, `native/data/example_devices/.gitkeep`, `native/data/migrations/README.md`
- [ ] 7.3 Run full suite: `ctest` (native unit+integration) + `pytest` + `./gradlew :app:connectedAndroidTest` (or at least `assembleDebug`); fix warnings
- [ ] 7.4 Manual QA checklist (§7.5) — record results in `docs/G0_QA.md` (optional)

### Parallelizable / Out-of-order (anytime after Phase 0)

- `tests/fixtures` + `tests/golden` creation
- `python/*` scaffolding
- Theme/colors polish
- `.gitignore` updates for `build/`, `*.sfproj.bak.*`, `logs/`

---

## 9. Definition of Done (Gate G0)

G0 is **done** when **all** of the following are true on `main`:

1. **Builds:** `./gradlew :app:assembleDebug` and `cmake --build native/build && ctest` pass on CI (Linux + Android NDK r26).
2. **Launches:** App installs on API 26+ emulator/device, `MainActivity` renders `HomeScreen` without crash.
3. **Project lifecycle:** User can create → save → kill → open project; instrumented test `ProjectStorageTest` passes.
4. **Schema:** `tests/unit/test_schema_validate` and `tests/python_tests/test_schema_py` pass; `project_schema.json` is the single source of truth (no drift between native and Python).
5. **Versioning:** `test_version` + `test_migration` pass; opening a `v0` fixture migrates to `v1` and writes `.bak.v0`.
6. **Diagnostics:** `test_diagnostics` passes; `adb logcat -s SF` shows startup logs; `sf_flush_logs()` writes `filesDir/logs/sf.log`; crash handler writes `crash.log` on uncaught exception.
7. **Health:** `sf_project_health_check` returns `ok` for empty valid project and `error` for corrupt fixture; UI shows chip.
8. **No G1+ leakage:** `native/src/{acoustics,arrays,dsp,graph,power,render,measurement,optimization}/*` contain only stub `CMakeLists.txt` and no logic; screens beyond Home/ProjectList are placeholders.
9. **Docs:** `docs/PLAN_G0.md` (this file) is checked in; `native/data/schemas/project_schema.json` is validated.

**Exit artifact:** Tag `g0-complete` and attach `*.sfproj` example + `adb logcat` snippet to release notes.

---

### Appendix — Quick Reference: Files to Create vs. Edit (Checklist)

**EDIT (existing stubs to flesh):**
`app/build.gradle.kts`, `app/CMakeLists.txt`, `native/CMakeLists.txt`, `native/src/core/CMakeLists.txt`, `native/include/soundforge/sf_types.h`, `sf_version.h`, `sf_project.h`, `sf_diagnostics.h`, `native/src/core/project.cpp`, `version.cpp`, `diagnostics.cpp`, `app/ui/MainActivity.kt`, `SoundForgeApplication.kt`, `app/ui/home/HomeScreen.kt`, `app/ui/project/ProjectListScreen.kt`, `app/ui/scene/SceneEditorScreen.kt`, `app/ui/signal/SignalEditorScreen.kt`, `app/ui/mixer/MixerScreen.kt`, `app/ui/dsp/DspChainScreen.kt`, `app/ui/prediction/PredictionScreen.kt`, `app/ui/measurement/MeasurementScreen.kt`, `app/ui/training/TrainingScreen.kt`, `app/ui/reports/ReportsScreen.kt`

**NEW (must be created for G0):**
`app/settings.gradle.kts`, `app/ui/navigation/NavGraph.kt`, `Routes.kt`, `app/ui/theme/Theme.kt`, `Color.kt`, `app/ui/project/ProjectViewModel.kt`, `NewProjectDialog.kt`, `app/platform/diagnostics/SfLogger.kt`, `CrashHandler.kt`, `app/platform/storage/StorageRepository.kt`, `FileStorageRepository.kt`, `ProjectFile.kt`, `app/platform/permissions/PermissionManager.kt`, `app/platform/audio/AudioCapabilityProbe.kt`, `app/platform/usb/UsbManagerStub.kt`, `app/platform/bridge/NativeBridge.kt`, `app/src/main/cpp/jni_bridge.cpp`, `native/include/soundforge/sf_schema.h`, `sf_migration.h`, `native/src/core/schema.cpp`, `migration.cpp`, `json_codec.cpp`, `uuid.cpp`, `version_gen.h.in`, `native/third_party/nlohmann/json.hpp`, `native/data/schemas/project_schema.json`, `native/data/migrations/manifest.json`, `python/pyproject.toml`, `python/soundforge_py/__init__.py`, `schema.py`, `migrate.py`, `tests/CMakeLists.txt`, `tests/fixtures/*.json` (4 files), `tests/golden/schema_golden_v1.json`, `tests/unit/*.cpp` (6 files), `tests/integration/*.cpp` (2 files), `tests/python_tests/test_schema_py.py`, `app/src/androidTest/.../ProjectStorageTest.kt`, `NavigationSmokeTest.kt`

**STUB (empty but must compile):**
`native/src/graph|dsp|audio|acoustics|arrays|power|render|measurement|optimization/CMakeLists.txt`, `python/data_import/__init__.py`, `python/reference/__init__.py`, `python/regression/__init__.py`, `native/data/example_devices/.gitkeep`

> Total G0 net new files: ~35–40. Keep each file small and single-purpose; no file >300 LOC in G0.

