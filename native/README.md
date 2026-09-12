# SoundForge Native Core (`native/`) — G0

Portable C++17 core: canonical project data model, JSON codec, schema
validation, and the v0→1 migration (docs/PLAN_G0.md §2.4). Built with CMake;
the Android app consumes it through the C ABI in `include/soundforge/sf_project.h`.
Audio/DSP/acoustics/render are G1+ stubs under `src/` — no engine logic yet.

`third_party/nlohmann/json.hpp` is **vendored** (single header, no submodules,
no network fetch) so NDK and CI builds stay hermetic and reproducible.

Subfolders:
- `data/schemas/` — canonical JSON Schema (`project_schema.json`, single source of truth)
- `data/migrations/` — migration descriptors (`manifest.json`)
- `data/example_devices/` — sample device data (future)
