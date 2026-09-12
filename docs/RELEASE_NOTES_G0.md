# SoundForge — Gate G0 Release Notes

**Tag:** `g0-complete`
**Engine version:** `0.1.0-g0`
**Date:** 2026-09-12

## Verification matrix (docs/PLAN_G0.md §9)

| # | Criterion | Status |
|---|---|---|
| 1 | `cmake --build native/build && ctest` | ✅ 27/27 pass |
| 1 | `./gradlew :app:assembleDebug` (Android) | ⚠️ not verifiable — no Android SDK/NDK in build env; static review only |
| 2 | Launches on API 26+ emulator | ⚠️ not verifiable — no emulator/device |
| 3 | `ProjectStorageTest` lifecycle | ⚠️ not verifiable — Android instrumented test |
| 4 | Schema unit tests + no native↔Python drift | ✅ `test_schema_validate` + `test_schema_py` pass; `project_schema.json` byte-identical to `tests/golden/schema_golden_v1.json` |
| 5 | Versioning + migration | ✅ `test_version` + `test_migration` pass; v0→v1 migrate + `.bak.v0` written on file open (`MigrationViaFile`) |
| 6 | Diagnostics | ✅ `test_diagnostics` pass; `sf_flush_logs(path)` writes `sf.log`. `adb logcat -s SF` / crash log: ⚠️ device-only |
| 7 | Health check | ✅ `ok` for valid, `error` for corrupt fixture |
| 8 | No G1+ leakage | ✅ stub dirs contain only `CMakeLists.txt`; screens beyond Home/ProjectList are placeholders |
| 9 | Docs + schema validated | ✅ `docs/PLAN_G0.md` checked in; schema JSON validates |

**Python:** `pytest tests/python_tests` — 4/4 pass.

## Example project

Canonical v1 example: `tests/fixtures/project_minimal_v1.json` (migrated v0 fixture: `project_minimal_v0.json`).

## Known G0 deviations (accepted)

- Gradle root is repo-root (`settings.gradle.kts` + `build.gradle.kts` with `include(":app")`) instead of `app/`-scoped; `./gradlew :app:assembleDebug` from repo root matches gate wording.
- `sf_last_error_global()` is populated by all ABI error paths (single `set_last_error` funnel).
- `native/src/core/project.cpp` is 378 LOC (appendix advisory >300).
- `adb logcat -s SF` snippet: unavailable — no Android device in dev environment.