# SoundForge — Gate G1 Plan: Scene & Venue Editing + Geometry Kernels

> **Status:** DRAFT (authored by orchestrator — planner/oracle lanes unavailable: `Model unavailable: opencode-go/*` at authoring time). Scheduled: adversarial review by @planner/@oracle once models recover.
> **Supersedes:** `docs/PLAN_G0.md` for G1 scope only. G0 remains the contract for everything not changed here.

---

## 1. Scope

### 1.1 Goal
The project is no longer a read-only skeleton. G1 delivers the first real editing
surface: the user can edit the scene and its venue, and the native core enforces
geometric sanity (coordinates inside the room, dimensional validation). The
round-trip create → edit scene/venue → save → reopen → edit survives with
schema v2 + v1→v2 migration, all under the existing no-drift schema discipline.

### 1.2 In scope (G1 only)
- **Schema v2** — additive fields only, no renames/removals of v1 keys:
  - `venue.dimensions: {widthM, depthM, heightM}` (positive numbers, meters)
  - `scene.geometry: {center:{x,y,z}, listening:{x,y,z}}` (meters, room-local coords)
- **Geometry kernels** (native, in `native/src/core/`) — bounded set:
  `sf_geo_validate_box`, `sf_geo_point_in_box`, `sf_geo_distance`, `sf_geo_max_dimension`.
  Each validates its inputs; errors via `set_last_error` + `SF_E_*` codes.
- **Scene/venue editing logic** (native ABI, additive):
  `sf_project_rename`, `sf_venue_rename` (+ `sf_venue_set_dimensions`),
  `sf_scene_set_geometry`. All mutators write an audit entry `*.update`-style
  (`project.rename`, `venue.update`, `scene.update`) and bump `modifiedAt`.
- **Kotlin/Compose editing UI**: `SceneEditorScreen.kt` becomes a real editor
  (scene name, venue picker, geometry numeric fields with native-validated
  feedback). NEW `VenueScreen.kt` (venue name + dimensions). Typed mirrors
  `SceneKt`/`VenueKt` (G0 deferred this to G1; UI now needs field-level editing).
- **NewProjectDialog persists the venue preset** (G0 captured it as UI-only
  state — see plan §6.3): selecting "Warehouse" sets the venue name at creation.
- **Migration v1→v2** + stepwise chain (0→1→2) in `sf_migrate_json`/`open()`.

### 1.3 Explicitly out of scope (G2+ — must stay stub/unchanged)
- Signal graph evaluation, routing, mixer engine (`graph/*`, `SignalEditorScreen`,
  `MixerScreen`) — **G2**.
- DSP chains, acoustics simulation, array optimization, measurement capture,
  training runtime — **G3–G9**.
- `audioAssets` `*.sfasset` binary sidecars — **deferred to G2** (decision in §6.5);
  v2 keeps `data.localPath` semantics + health-check Warning.
- Envelope `parentId`/`refs` — deferred (no child objects in G1; single venue +
  single scene per document stays).
- Multi-scene / multi-venue documents (schema keeps `venue`/`scene` as singletons;
  a collection refactor is a future schema change with its own gate).
- Hardware probing (AAudio/Oboe) — remains scaffolding.

### 1.4 Gating rule
Same as G0: files listed as *stub* are ≤20 LOC, reachable from `include/`, and
carry `// G0: stub — G1+ implements` (unchanged for G2+ stubs). Files listed as
*new* implement their bounded contract only. No file >300 LOC (advisory).

---

## 2. Architecture Changes

```
                    ┌──────────────────────────────────────────────┐
   Kotlin UI         │  SceneEditorScreen.kt  VenueScreen.kt (NEW)│
                    │  ProjectViewModel.kt (+SceneKt/VenueKt)    │
                    └───────────────┬──────────────────────────────┘
                                    │ NativeBridge.kt (+3 funs)
                                    ▼
   JNI bridge (jni_bridge.cpp, +3 exports)   libsfcore.so
                    │
   native core ─────┼── sf_project.h  (rename, venue/scene mutators)
                    ├── sf_geometry.h (NEW: kernels, sf_*_geo_*)
                    ├── schema.cpp    (v2 validation rules)
                    ├── migration.cpp (v1→v2 + stepwise chain)
                    └── geometry.cpp  (NEW: kernel impls, unit-testable)
```

- Kotlin still treats the document as opaque JSON; **all validation lives in
  native**. Kotlin assembles only the small typed payloads its edit forms need
  (a few doubles + strings) and reads success/failure from return codes +
  `lastError`, exactly like G0.
- Schema remains the single source of truth. Python `schema.py` reads the same
  JSON file → drift-free by construction; `migrate.py` gains the v1→v2 step.
- Golden file moves to `tests/golden/schema_golden_v2.json`
  (byte-identical to the updated `native/data/schemas/project_schema.json`).

---

## 3. Schema v2 (mini-spec)

Single source of truth: edit `native/data/schemas/project_schema.json` first.
Full JSON not repeated here — only the diffs against v1.

### 3.1 `venue` (was `{id, name}`)
```json
"venue": {
  "type": "object",
  "required": ["id", "name", "dimensions"],
  "properties": {
    "id":   { "type": "string", "format": "uuid" },
    "name": { "type": "string" },
    "dimensions": {
      "type": "object",
      "required": ["widthM", "depthM", "heightM"],
      "properties": {
        "widthM":  { "type": "number", "exclusiveMinimum": 0 },
        "depthM":  { "type": "number", "exclusiveMinimum": 0 },
        "heightM": { "type": "number", "exclusiveMinimum": 0 }
      },
      "additionalProperties": false
    }
  },
  "additionalProperties": false
}
```

### 3.2 `scene` (was `{id, name, venueRef}`)
```json
"scene": {
  "type": "object",
  "required": ["id", "name", "venueRef", "geometry"],
  "properties": {
    "id":       { "type": "string", "format": "uuid" },
    "name":     { "type": "string" },
    "venueRef": { "type": "string", "format": "uuid" },
    "geometry": {
      "type": "object",
      "required": ["center", "listening"],
      "properties": {
        "center":   { "$ref": "#/$defs/point3" },
        "listening": { "$ref": "#/$defs/point3" }
      },
      "additionalProperties": false
    }
  },
  "additionalProperties": false
}
```
with a new shared def:
```json
"point3": {
  "type": "object",
  "required": ["x", "y", "z"],
  "properties": {
    "x": { "type": "number" },
    "y": { "type": "number" },
    "z": { "type": "number" }
  },
  "additionalProperties": false
}
```

### 3.3 Semantics (native validator must enforce these too)
- Coordinates are room-local meters, origin at the venue's front-left floor
  corner; `x∈[0,widthM]`, `y∈[0,depthM]`, `z∈[0,heightM]`.
- Structural validation (schema.cpp `validate_doc_json`) checks types/required
  only — same posture as v1. Bounded-geometry checks (inside-room) are **health
  level** (warning), *not* schema errors, so non-conforming legacy files still open.
- Defaults injected by migration (below) are guaranteed in-bound so a freshly
  migrated v1 file is fully "healthy".

---

## 4. Native Core Contracts

### 4.1 Geometry kernels — NEW `native/include/soundforge/sf_geometry.h` + `native/src/core/geometry.cpp`
All kernels pure + thread-safe; no handle. Errors via `set_last_error` (which
feeds both thread-local and global channels — G0 fix) + return codes. Never throw
across ABI (wrap in `SF_CATCH_ERRORS`).

```c
/* Room box (meters). dims: {widthM, depthM, heightM} > 0. */
sf_result_t sf_geo_validate_box(double width, double depth, double height);

/* Point-in-room: TRUE when 0 <= x <= width etc. (inclusive), else FALSE.
   Returns SF_E_INVALID_ARG when box invalid. */
sf_result_t sf_geo_point_in_box(double x, double y, double z,
                                double width, double depth, double height,
                                int* out_inside);

/* Euclidean distance, meters. */
sf_result_t sf_geo_distance(double x1, double y1, double z1,
                            double x2, double y2, double z2,
                            double* out_m);

/* Largest linear dimension (for venue summary). */
sf_result_t sf_geo_max_dimension(double width, double depth, double height,
                                 double* out_m);
```

### 4.2 Document mutators — extend `native/include/soundforge/sf_project.h`
Additive only; existing signatures untouched. Every mutator:
- rejects `p == nullptr` → `SF_E_INVALID_ARG` + `("rename: null handle")`-style
  message;
- propagates `lastError` onto the handle (*and* thread-local, via the existing
  `set_handle_error` funnel);
- appends an audit entry `{ts, actor:"user", action:"project.rename"|"venue.update"|
  "scene.update", objectId:<entity id>, detail:"<field>=<value>"}`;
- bumps `project.modifiedAt = now_iso8601()`.

```c
sf_result_t sf_project_rename(sf_project_t* p, const char* new_name);
/* new_name non-empty; caps at 200 chars (matching v1 schema's string length). */

sf_result_t sf_venue_rename(sf_project_t* p, const char* new_name);
sf_result_t sf_venue_set_dimensions(sf_project_t* p,
                                    double width, double depth, double height);
/* dimensions validated via sf_geo_validate_box first; on SF_OK also snap
   scene.geometry into bounds: clamp center/listening to [0, dim]. */

sf_result_t sf_scene_set_geometry(sf_project_t* p,
                                  double cx, double cy, double cz,
                                  double lx, double ly, double lz);
/* Requires p's venue.dimensions present+valid (else SF_E_SCHEMA);
   validates both points via sf_geo_point_in_box; rejects out-of-room
   points with last_error "scene.update: geometry outside venue bounds". */
```

### 4.3 JNI bridge — additive
`app/src/main/cpp/jni_bridge.cpp` gains exports 13–16 (prefix
`Java_id_soundforge_pastudio_platform_bridge_NativeBridge_`); `NativeBridge.kt`
gains four mirrors, still *one external fun per C entry, no logic*:

```kotlin
external fun renameProject(handle: Long, newName: String): Int    // SF_* code
external fun renameVenue(handle: Long, newName: String): Int      // added in DoD sweep (c48ebc5)
external fun setVenueDimensions(handle: Long, w: Double, d: Double, h: Double): Int
external fun setSceneGeometry(handle: Long, cx: Double, cy: Double, cz: Double,
                              lx: Double, ly: Double, lz: Double): Int
```
Export 16 (`renameVenue`) closes a gap found in the DoD sweep: §6.3's venue
name field needs a venue-name commit path, and §4.2 already defines
`sf_venue_rename` — without the export, `updateVenue` renamed the *project*
instead, corrupting the three-name document model.
`Int` result codes keep the G0 "empty-string means error, check lastError"
pattern uniformly applicable (bridge stays thin; Kotlin callers map `!=SF_OK` →
`lastError(handle)`).

### 4.4 Versioning
- `SF_SCHEMA_VERSION` 1 → **2** in `native/include/soundforge/sf_version.h`.
- `sf_is_compatible(sv)` semantics unchanged formula (sv ≤ current); with v2:
  `(0)=1 (1)=1 (2)=1 (3)=0 (-1)=0`. Update `tests/unit/test_version.cpp` and the
  Python version constant to `0.1.0-g1` in `python/soundforge_py/migrate.py`
  (engine version carried by migrated docs).
- `sf_engine_version()` macro — untouched (release machinery).

---

## 5. Migration v1→v2

### 5.1 Technique — mirror the v0→v1 style in `native/src/core/migration.cpp`
- Keep `migrate_doc_inplace` per-step (`(0,1)`, `(1,2)`), and make
  `sf_migrate_json` chain steps when `to_ver > from_ver + 1`:
  `for (v = from; v < to; ++v) migrate_doc_inplace(j, v, v+1)`.
  Same for the `open()` path in `project.cpp` (already migrates to
  `SF_SCHEMA_VERSION`; the loop makes it work for any gap). `SF_E_VERSION`
  (downgrade), no-op `from==to`, and `peek_schema_version >= target`
  idempotence checks stay.
- `(1,2)` step:
  1. If `peek_schema_version(j) >= 2` → return `SF_OK` (idempotent).
  2. `j["schemaVersion"] = 2; j["engineVersion"] = sf_engine_version();`
  3. `venue.dimensions` — unless already valid (`sf_geo_validate_box`), inject
     `{widthM:12.0, depthM:10.0, heightM:4.0}` (default "small club room").
  4. `scene.geometry` — unless present with both points, inject
     `center = {width/2, depth/2, height/2}` (room middle),
     `listening = center` (same point; the editor can move it).
  5. Append audit entry `{ts, "system", "project.migrate", scene.id,
     "migrated 1->2"}` (same shape as 0→1).
- Backup naming on file open: `*.sfproj.bak.v1` for a v1 source (G0 wrote
  `.bak.v0`; extend the existing copy-old-files logic to version-generic naming
  `.{from}.bak` → plan G0 used `.bak.v0`; keep that scheme, i.e. suffix = source
  schemaVersion). Update `test_project_io.cpp` migration test expectations.

### 5.2 Golden + fixtures + Python
- New golden `tests/golden/schema_golden_v2.json` — byte-identical copy of the
  updated schema (regenerate via the existing `schema_golden` fixture workflow).
  Old `schema_golden_v1.json` is kept as an immutable v1 reference for the
  migration tests, or archived to `tests/golden/archive/schema_golden_v1.json` —
  **decide at review; do not delete silently**.
- New fixtures: `tests/fixtures/project_minimal_v2.json`,
  `tests/fixtures/project_full_empty_v2.json`. Keep v0/v1 fixtures (migration
  inputs). `project_corrupt.json` unchanged.
- Python `migrate.py`: add step `(1,2)` mirroring §5.1 (same injection rules);
  bump version constant. `schema.py` needs no change (reads JSON).
- Drift guard (DoD): `diff native/data/schemas/project_schema.json
  tests/golden/schema_golden_v2.json` must be empty, and the Python validator
  must accept/emit the same v2 docs (roundtrip byte-check in pytest).

---

## 6. Kotlin / Compose Changes

### 6.1 Typed mirrors — NEW `app/ui/scene/SceneKt.kt`, NEW `app/ui/venue/VenueKt.kt`
Small immutable data classes (read-only views over opaque native JSON, parsed
from `projectToJson` output on the IO dispatcher — same posture as
`parseMeta`): `SceneKt(id,name,venueRef,center:Pt3,listening:Pt3)` and
`VenueKt(id,name,widthM,depthM,heightM)`; shared `data class Pt3(x,y,z)`.
G0's `ProjectMetaKt` stays as the list-level summary; these are the editor
surfaces.

### 6.2 `SceneEditorScreen.kt` — real editor (EDIT)
`Scaffold` + TopAppBar("Scene editor — {projectName}") surrounding:
- scene name `OutlinedTextField` → `renameProject` on blur/confirm
- venue picker `ExposedDropdownMenuBox` (venue is a singleton; shows venue name +
  "Edit venue…" → navigates to `venue/{projectId}`)
- geometry form: 6 numeric fields (center x/y/z, listening x/y/z, meters) →
  `setSceneGeometry`, field-level `supportingText` shows the native
  `lastError` on failure (e.g. "outside venue bounds")
- stays `projectId: String?`-parameterized; falls back to the "no project open"
  affordance when `null` (matching G0 placeholder posture).
Responsive layout + Material3 theming preserved from G0 theme files.

### 6.3 `VenueScreen.kt` — NEW screen
- venue name field, dimensions fields (w/d/h meters) →
  `setVenueDimensions`; shows validation errors inline; "Back to scene" nav.
- Added to `NavGraph.kt` route map + `Routes.kt` (`VENUE_EDITOR = "venue/{projectId}"`).

### 6.4 `NewProjectDialog.kt` + `ProjectViewModel.kt` — persist the preset (EDIT)
- Dialog stays visually identical; `onConfirm` now passes `(name, venuePreset)`.
- `ProjectViewModel.create(name, venueName)`: native `projectCreate(name, null)`
  then `renameProject(newHandle, name)` + `setVenueDimensions` default
  `(12,10,4)` only if a preset named after a built-in profile is selected
  (G1: presets map to default dims; free-form venue names keep dims default).
- `ProjectViewModel` gains `fun updateScene(geometry)/fun updateVenue(...)`
  helpers (thin wrappers over bridge + state refresh via `projectToJson`).

### 6.5 audioAssets sidecars — DECISION: defer to G2
Rationale: sidecars only matter once the mixer/signal pipeline references
assets (G2); adding blob plumbing now would force a schema/keying decision we
cannot validate without G2 semantics. v2 keeps `data.localPath` + health
Warning behavior from G0. Recorded here so G2 does not re-litigate.

---

## 7. Phased Implementation Order

Work in this order; each phase leaves the tree buildable + all suites green.

| # | Phase | Files | Evidence |
|---|---|---|---|
| P1 | Schema v2 + golden/fixtures | EDIT `native/data/schemas/project_schema.json`; NEW golden v2, fixtures minimal_v2/full_empty_v2; EDIT `tests/unit/test_schema_validate.cpp`, `test_version.cpp` | ctest schema+version; pytest `test_schema_py`; drift diff |
| P2 | Migration chain + v1→v2 | EDIT `migration.cpp`, `project.cpp` (open loop), `python/soundforge_py/migrate.py`; EDIT `tests/unit/test_migration.cpp`, `tests/unit/test_version.cpp`, `tests/integration/test_project_io.cpp`, `tests/python_tests/*` | ctest migration+integration; pytest v1→v2; `.bak.v1` file check |
| P3 | Geometry kernels | NEW `sf_geometry.h`, `geometry.cpp`, `tests/unit/test_geometry.cpp` | ctest geometry |
| P4 | Document mutators | EDIT `sf_project.h`, `project.cpp` (4 mutators + audit), `tests/unit/test_scene_venue.cpp` NEW | ctest scene/venue roundtrip; audit entries; error codes |
| P5 | JNI + Kotlin models | EDIT `jni_bridge.cpp` (+3 exports), `NativeBridge.kt` (+3 funs); NEW `SceneKt.kt`, `VenueKt.kt` | static review (no SDK); exported-symbols grep check (13–15) |
| P6 | Editing UI | EDIT `SceneEditorScreen.kt`, `ProjectViewModel.kt`, `NewProjectDialog.kt`, `NavGraph.kt`, `Routes.kt`; NEW `VenueScreen.kt`; EDIT `app/src/androidTest/.../NavigationSmokeTest.kt`; NEW `SceneEditSmokeTest.kt` | static review only |
| P7 | Docs + DoD sweep | EDIT `docs/PLAN_G1.md` (this file), `docs/RELEASE_NOTES_G1.md` NEW; gap-check Appendix | full suites + drift + release notes |

**Gate status (2026-09-12):** P1+P2 `7b15d28` · P3 `c1b5849` · P4 `8c7be60` ·
P5 `d449baf` · P6 `0ecfcf8` · DoD-sweep fixes `c48ebc5` · P7 = this commit.
DoD results in §9; accepted deviations in `docs/RELEASE_NOTES_G1.md`.

## 8. Verification (realistic for this environment)

```bash
# 1. Native (Termux/proot: -j2, generous timeouts)
cmake --build native/build -j2 && ctest --test-dir native/build --output-on-failure

# 2. Python
python3 -m pytest tests/python_tests -q

# 3. Schema drift (must print nothing / exit 0)
diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json
# Python mirror must accept the canonical doc (validate_project validates
# project DOCUMENTS, not the schema file itself):
python3 -c "import sys,json; sys.path.insert(0,'python'); \
import soundforge_py.schema as s; d=json.load(open(\
'tests/fixtures/project_minimal_v2.json')); assert s.validate_project(d)==[]; \
print('mirror validates v2 fixture')"

# 4. Android (static only — no SDK/NDK in dev container; documented limitation)
grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' \
  app/src/main/cpp/jni_bridge.cpp   # expect 16 (12 G0 + 4 G1: renameProject,
                                    # renameVenue, setVenueDimensions, setSceneGeometry)
```

## 9. Definition of Done (Gate G1)

All true on `main`:

1. **Builds:** `cmake --build native/build && ctest --test-dir native/build` and
   `./gradlew :app:assembleDebug` (CI, Linux + Android NDK r26 — local:
   native+pytest green, Android static review).
2. **Schema v2:** `test_schema_validate` + `test_schema_py` pass;
   `project_schema.json` byte-identical to `schema_golden_v2.json`; Python mirror
   validates the same docs (no drift).
3. **Migration:** v1 fixture opens → `schemaVersion=2`, writes `.bak.v1`;
   v0 opens via chained 0→1→2; `sf_migrate_json(…,0,2)` stepwise; downgrade
   still `SF_E_VERSION`.
4. **Geometry:** kernels unit-pass; invalid box rejected; point-in-room correct
   at inclusive bounds; maximum dimension exact.
5. **Editing round-trip:** rename scene/venue + dimensions + geometry persist
   through save/reopen (integration test).
6. **Bounds enforcement:** out-of-room geometry → `SF_E_SCHEMA` +
   `last_error` set on handle + thread-local; health check reports Warning (not
   Error) if a legacy doc has out-of-bounds geometry.
7. **Audit trail:** each successful mutator appends its audit entry; ring-flush
   test still green.
8. **No G2+ leakage:** `graph|dsp|audio|acoustics|arrays|power|render|
   measurement|optimization/*` remain stub-only; `SignalEditorScreen`/
   `MixerScreen` remain placeholders; no `*.sfasset` code lands.
9. **Docs:** this file + `docs/RELEASE_NOTES_G1.md` checked in; tag `g1-complete`.

**Result (2026-09-12):** 1 ✅ native 41/41 (Android ⚠️ static-only, export
grep 16) · 2 ✅ drift IDENTICAL + pytest · 3 ✅ migration chain + `.bak.v1` ·
4 ✅ geometry kernels · 5 ✅ editing round-trip · 6 ✅ mutator rejects oob
(`SF_E_SCHEMA`) + health Warning for legacy oob docs · 7 ✅ audit entries 4→7
actions · 8 ✅ no G2 leakage · 9 ✅ docs + tag `g1-complete`.

**Exit artifact:** tag `g1-complete`; release notes attach an example `.sfproj`
(v2 fixture) + `adb logcat -s SF` snippet (device-only, placeholder format).

---

## Appendix — Files to Create vs Edit (Checklist)

**EDIT:**
`native/data/schemas/project_schema.json`, `native/include/soundforge/sf_version.h`,
`sf_project.h`, `native/src/core/migration.cpp`, `project.cpp`, `schema.cpp`
(venue/scene dimension+geometry rules), `native/include/soundforge/sf_schema.h`
(v2 expected-keys), `app/src/main/cpp/jni_bridge.cpp`,
`app/platform/bridge/NativeBridge.kt`, `app/ui/scene/SceneEditorScreen.kt`,
`app/ui/project/ProjectViewModel.kt`, `NewProjectDialog.kt`,
`app/ui/navigation/NavGraph.kt`, `Routes.kt`,
`python/soundforge_py/migrate.py`, `python/soundforge_py/__init__.py` (version
const), `tests/unit/test_schema_validate.cpp`, `test_version.cpp`,
`test_migration.cpp`, `tests/integration/test_project_io.cpp`,
`tests/python_tests/test_schema_py.py`, `app/src/androidTest/.../NavigationSmokeTest.kt`,

**NEW:**
`native/include/soundforge/sf_geometry.h`, `native/src/core/geometry.cpp`,
`tests/unit/test_geometry.cpp`, `tests/unit/test_scene_venue.cpp`,
`tests/golden/schema_golden_v2.json`, `tests/fixtures/project_minimal_v2.json`,
`tests/fixtures/project_full_empty_v2.json`,
`app/ui/scene/SceneKt.kt`, `app/ui/venue/VenueKt.kt`, `app/ui/venue/VenueScreen.kt`,
`app/src/androidTest/.../SceneEditSmokeTest.kt`, `docs/RELEASE_NOTES_G1.md`.

**STUB (unchanged from G0):**
`native/src/{graph,dsp,audio,acoustics,arrays,power,render,measurement,optimization}/CMakeLists.txt`.

> Total G1: ~13 NEW, ~20 EDIT, 0 new STUB. Actual LOC (2026-09-12):
> `geometry.cpp` 81 ✓ · `SceneEditorScreen.kt` 276 ✓ · `ProjectViewModel.kt` 284 ✓ ·
> `VenueScreen.kt` 207 ✓ · `schema.cpp` 320 / `migrate.py` 354 / `project.cpp` 540
> exceed the ≤300 advisory (project.cpp was already 378 at G0; advisory only,
> not a gate criterion).

## Appendix B — Open Questions (for review gate)

1. Golden v1 handling: keep `schema_golden_v1.json` beside v2, or archive it?
   (Recommendation: archive — the v1 fixture + git history preserve it anyway.)
2. Should `renameProject` failures roll back nothing / leave handle valid?
   (Recommendation: pure mutator — return error, handle stays usable.)
3. NewProjectDialog preset→dimensions mapping table: hardcode 3 profiles or read
   from native? (Recommendation: hardcode 3 in `VenuePresets`, matching G0 UI.)
4. Health-check severity for out-of-bounds geometry: Warning (rec.) vs Error.

**Resolved in gate (2026-09-12):**

1. **KEPT** `schema_golden_v1.json` beside v2 — G0 release notes reference it,
   the drift test targets v2 only, and git history preserves v1 anyway.
2. **Pure mutator** — `renameProject` failure leaves the handle valid and the
   document unchanged (test `SceneVenue.RenameRejects`); rollback would need a
   transactional layer G1 doesn't need.
3. **Hardcoded** 3 profiles in `BuiltInVenuePresetDims` (Kotlin); "Small Club"
   mirrors `SF_ROOM_DEFAULT_*` (`sf_internal.hpp` + `migrate.py` lockstep).
   Drift hazard acknowledged in RELEASE_NOTES_G1.md (feature constant, not
   schema).
4. **Error on mutator write** (`SF_E_SCHEMA` + `last_error`) and **Warning in
   `sf_project_health_check`** for legacy out-of-bounds docs (`c48ebc5`) —
   schema validation stays structural-only so such docs remain openable and
   repairable in the editor.

>The owed specialist review of this appendix + P4/P5 (planner/oracle) and of
> the JNI surface (security-reviewer) is deferred: `opencode-go/*` provider
> models were unavailable throughout the gate. Re-dispatch on recovery.