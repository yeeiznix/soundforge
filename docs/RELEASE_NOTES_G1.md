# SoundForge — Gate G1 Release Notes

**Tag:** `g1-complete`
**Engine version:** `0.1.0-g1`
**Date:** 2026-09-12

## Gate commits (all on `main`)

| Phase | Commit | Content |
|---|---|---|
| P1+P2 | `7b15d28` | Schema v2 (venue.dimensions, scene.geometry) + golden v2 + migration chain 0→1→2 + Python mirror |
| P3 | `c1b5849` | Geometry kernels (`sf_geometry.h`, `geometry.cpp`) |
| P4 | `8c7be60` | 4 mutators (rename×2, set_venue_dimensions, set_scene_geometry) + audit 4→7 + tests |
| P5 | `d449baf` | JNI exports 13–16 + `NativeBridge.kt` mirrors + `SceneKt.kt`/`VenueKt.kt` |
| P6 | `0ecfcf8` | Scene/venue editing UI (pure views) + `VenueScreen.kt` + smoke tests |
| DoD sweep | `c48ebc5` | Venue-name semantics fix (`renameVenue`), scene-name seeding from `project.name`, health-check Warning for legacy out-of-bounds geometry |

## Verification matrix (docs/PLAN_G1.md §9)

| # | Criterion | Status |
|---|---|---|
| 1 | `cmake --build native/build && ctest` | ✅ 41/41 pass |
| 1 | `./gradlew :app:assembleDebug` (Android) | ⚠️ not verifiable — no Android SDK/NDK in build env; static review only + export grep (16) |
| 2 | Schema v2, byte-identical, no drift | ✅ `project_schema.json` byte-identical to `tests/golden/schema_golden_v2.json`; `test_schema_validate` + `test_schema_py` pass |
| 3 | Migration chain | ✅ v1 fixture opens → `schemaVersion=2` + `.bak.v1`; v0 via chained 0→1→2; stepwise `sf_migrate_json(…,0,2)`; downgrade `SF_E_VERSION` |
| 4 | Geometry kernels | ✅ `test_geometry` pass; invalid box rejected; point-in-room correct at inclusive bounds; maximum dimension exact |
| 5 | Editing round-trip | ✅ rename scene(=project)/venue + dimensions + geometry persist through save/reopen (`SceneVenue` suite incl. `RoundTripPreservesEdits`) |
| 6 | Bounds enforcement | ✅ out-of-room geometry → `SF_E_SCHEMA` + `last_error`; health check reports **Warning** (not Error) for legacy out-of-bounds docs (`HealthWarnsOnLegacyOutOfBoundsGeometry`) |
| 7 | Audit trail | ✅ every mutator appends `{actor:"user", …}` entry + bumps `modifiedAt`; ring-flush test green |
| 8 | No G2+ leakage | ✅ stub dirs unchanged; `SignalEditorScreen`/`MixerScreen` placeholders; no `*.sfasset` code |
| 9 | Docs + tag | ✅ `docs/PLAN_G1.md` updated + this file + tag `g1-complete` |

**Python:** `pytest tests/python_tests` — 5/5 pass.

## Example project

Canonical v2 example: `tests/fixtures/project_minimal_v2.json` (1 371 B).
Full-empty v2: `tests/fixtures/project_full_empty_v2.json`.

`adb logcat -s SF` snippet: ⚠️ device-only — no emulator/device in dev
environment (placeholder for CI device lane).

## Known G1 deviations (accepted)

- **JNI export count 16 vs plan's 15.** PLAN_G1 §4.3 scoped exports 13–15, but
  §4.2 already defined `sf_venue_rename` and §6.3 needs a venue-name commit
  path; without an export, `updateVenue` renamed the *project* instead of the
  venue (corrupting the three-name document model). Closed additively in
  `c48ebc5`: export 16 `renameVenue`. §4.3/§8.4 amended to 13–16.
- **LOC advisory** (non-gate): `project.cpp` 540 (inherited over-limit from G0:
  378), `schema.cpp` 320, `migrate.py` 354 — advisory target was ≤300.
  `geometry.cpp` 81, `SceneEditorScreen.kt` 276, `ProjectViewModel.kt` 284,
  `VenueScreen.kt` 207 all within target.
- **Android static-only:** `assembleDebug`, instrumented tests and logcat are
  unverifiable in this environment (no SDK/NDK). Host-side evidence: export
  grep `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'`
  → **16**; `SceneEditSmokeTest`/`NavigationSmokeTest` compose the null-project
  affordance only (no `libsfcore.so` on host). `NavigationSmokeTest` needed no
  changes (plan listed it as EDIT; scene/venue routes are device-only).
- **Owed specialist review — deferred.** The checklist review pass
  (planner/oracle on PLAN_G1 Appendix B + P4/P5; security-reviewer on the JNI
  surface) could not run: `opencode-go/*` provider models were unavailable
  throughout P1–P6 and remain so (last attempt 2026-09-12 failed with "Model
  unavailable"). Appendix B decisions are recorded in PLAN_G1.md; the JNI
  surface was written to the G0 boundary discipline (thin passthrough,
  `to_handle`/`to_std` guards, exception fence returning `SF_E_INVALID_ARG`).
  Re-dispatch this review on provider recovery; gate does not block on it.
- **§8.3 verification command corrected:** `validate_project` validates
  project *documents*, not the schema file; the mirror check now runs against
  the canonical v2 fixture (equivalent to `test_validate_minimal_v2`).
- **Single-name model (per §6.2 literal):** the scene editor's name field
  edits the *project* name (`renameProject`); `scene.name` stays the creation
  default ("Default Scene") since no `sf_scene_rename` mutator exists by
  design. TopAppBar title uses `project.name` as specified.
- **Venue rename path:** `VenueScreen`'s name field commits via `renameVenue`
  (JNI 16) + `setVenueDimensions` in one `updateVenue` call; the plan's §6.3
  phrase "→ setVenueDimensions" alone could not rename — closed via §4.2's
  existing mutator.
- **Preset→dims hardcode:** `BuiltInVenuePresetDims` (3 profiles) lives in
  Kotlin; "Small Club" mirrors native `SF_ROOM_DEFAULT_*` in `sf_internal.hpp`
  + `migrate.py`. Drift hazard acknowledged (Appendix B #3): the 3 profiles
  are feature constants, not schema.