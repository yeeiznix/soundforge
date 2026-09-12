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
| 1 | `cmake --build native/build && ctest` | ✅ 41/41 at gate; **46/46 after g1.1** (5 new + 1 rewritten SceneVenue tests) |
| 1 | `./gradlew :app:assembleDebug` (Android) | ⚠️ not verifiable — no Android SDK/NDK in build env; static review only + export grep (16) |
| 2 | Schema v2, byte-identical, no drift | ✅ `project_schema.json` byte-identical to `tests/golden/schema_golden_v2.json`; `test_schema_validate` + `test_schema_py` pass |
| 3 | Migration chain | ✅ v1 fixture opens → `schemaVersion=2` + `.bak.v1`; v0 via chained 0→1→2; stepwise `sf_migrate_json(…,0,2)`; downgrade `SF_E_VERSION` |
| 4 | Geometry kernels | ✅ `test_geometry` pass; invalid box rejected; point-in-room correct at inclusive bounds; maximum dimension exact |
| 5 | Editing round-trip | ✅ rename scene(=project)/venue + dimensions + geometry persist through save/reopen (`SceneVenue` suite incl. `RoundTripPreservesEdits`) |
| 6 | Bounds enforcement | ✅ out-of-room geometry → `SF_E_SCHEMA` + `last_error`; health check reports **Warning** (not Error) for legacy out-of-bounds docs (`HealthWarnsOnLegacyOutOfBoundsGeometry`) |
| 7 | Audit trail | ✅ every mutator appends `{actor:"user", …}` entry + bumps `modifiedAt`; ring-flush test green |
| 8 | No G2+ leakage | ✅ stub dirs unchanged; `SignalEditorScreen`/`MixerScreen` placeholders; no `*.sfasset` code |
| 9 | Docs + tag | ✅ `docs/PLAN_G1.md` updated + this file + tag `g1-complete` |

**Python:** `pytest tests/python_tests` — 5/5 at gate; **6/6 after g1.1** (added golden-v1 immutability guard).

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
  — **RESOLVED in g1.1:** the review was re-dispatched the same day after
  provider recovery; both @oracle and @security-reviewer completed. See the
  g1.1 section below for the findings disposition.
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

## g1.1 review follow-up (post-gate, `main`)

**Commits:** fixes+tests `d7183be` · docs (this file + PLAN_G1.md) — follow-up
to `g1-complete`. Gate tag unchanged; no gate-contract change (exports still
16, schema v2 still single source of truth).

**Review outcome:** the deferred specialist pass ran on 2026-09-12 after
provider recovery. @oracle's verdict: *no blocking defect in the native core,
schema, migration, or mutators — would not revoke the tag*; fixed-before-build
items were the two Android-side HIGHs (below). Batches:

| # | Finding (sev) | Disposition |
|---|---|---|
| 1 | Editor screens called native on the main thread, racing IO mutations (HIGH) | **Fixed** — screens are now pure views: `scene`/`venue`/`projectName` feed in from `UiState.Ready` via NavGraph; no `projectToJson`/`lastError` in composables |
| 2 | Handle data race + destroy-vs-commit window (HIGH) | **Fixed** — `@Volatile handle`/`lastEditError`, `nativeMutex` serializes all six handle-touching coroutines, `close()` joins in-flight work (`runBlocking { withLock { … } }`) before `projectDestroy` |
| 3 | §1.2 "preset sets venue name" unimplemented; duplicate rename audit per create (LOW-MED) | **Fixed** — §1.2 corrected to dimensions-only; redundant `renameProject` after `projectCreate` removed (kills the duplicate `project.rename` entry) |
| 4 | `schema_golden_v1.json` unguarded orphan (LOW) | **Fixed** — SHA-256 immutability guard in `test_schema_py` (`2a2b1ca…`) |
| 5 | Clamp floor 0 / snap un-audited (LOW) | **Fixed** — audit detail records "; scene geometry snapped to room" when the shrink clamp rewrites geometry (`ClampSnappedRecordsAuditDetail`), incl. the corner-parking floor-0 case |
| 6 | Non-finite point mislabeled "outside venue bounds" (LOW) | **Fixed** — finiteness check in `sf_geo_point_in_box`; `SF_E_INVALID_ARG` + "coordinates must be finite" on the mutator path |
| 7 | JNI: `projectDestroy` without exception fence (HIGH, security) | **Fixed** — try/catch + `log_boundary_exception` |
| 8 | JNI: `validateJson` OOM null misread as "valid" (MED, security) | **Fixed** — `NewStringUTF` null guard |
| 9 | JNI: `schemaVersion`/`isCompatible` silent catches (MED) | **Fixed** — exceptions now logged |
| 10 | JNI: `projectToJson` unbounded output → Java-heap OOM (MED) | **Fixed** — 16 MiB gate returns `""` (existing error contract) + log |
| 11 | Non-nothrow `new` at 2 codec sites (LOW) | **Fixed** — `std::nothrow` + `SF_E_NOMEM` |
| 12 | Test gaps + brittle fixture text surgery (LOW) | **Fixed** — 5 new tests (non-finite dims/points, inclusive-boundary write, 201-char venue name, clamp-snap detail); health test rewired to programmatic nlohmann mutation |

**Accepted residuals (documented, non-blocking):**

- **JSON parse depth (LOW-02, security):** vendored nlohmann 3.11.3 has no
  `max_depth` param, and its parse callback *discards* subtrees on `false`
  instead of failing (verified experimentally) — a depth gate is unsafe
  without patching third-party code. Deeply-nested crafted `.sfproj` could
  exhaust the parse stack. Accepted for the local-file threat model; G2 TODO.
- **Rename cap is bytes, not characters:** native `strlen(new_name) > 200`
  rejects ≤200-code-unit emoji-heavy names; UI has no length counter. G2 TODO
  (char-based cap or Kotlin pre-check).
- **Modified UTF-8 / embedded NUL:** a crafted name containing U+0000
  round-trips asymmetrically (JNI encodes as `0xC0 0x80`; `NewStringUTF`
  truncates at the decoded NUL). Not reachable from user keystrokes; local
  docs only. Documented, no code change.
- **No valid-handle registry:** double-destroy / use-after-destroy is an
  app-level bug class the JNI bridge cannot detect; the g1.1 mutex + 0L guard
  shrink the window (screen reloads no longer touch native at all).
  Registry deferred.
- **Blanket JNI `catch → SF_E_INVALID_ARG`:** masks genuine native bugs as
  "invalid argument" at the UI; exception messages are now logged, mapping
  refinement deferred.
- **Reseed-on-refresh semantics:** because screens seed from committed
  `UiState.Ready` values, a commit that refreshes state overwrites unapplied
  typing in other fields (committed-doc-is-truth). Deliberate trade for
  race-freedom; no silent data loss (doc is authoritative).