# SoundForge Python (`python/`) — G0

Reference & tooling package for SoundForge. **Not required for the G0 gate**
(docs/PLAN_G0.md §2.5); the layout exists so later gates can build on it.

- `soundforge_py/schema.py` — validates project JSON against the canonical
  schema, `native/data/schemas/project_schema.json` (single source of truth),
  using `jsonschema` (Draft 2020-12). CLI: `python3 python/soundforge_py/schema.py <file.json>`.
- `soundforge_py/migrate.py` — Python mirror of the C++ v0→1 migrator
  (`native/src/core/migration.cpp`); keep key names & defaults in lockstep.
- `data_import/`, `reference/`, `regression/` — placeholder packages.

The **reference engine is a G0 stub**: the C++ core only implements project
create/open/save/validate/migrate. DSP, acoustics, simulation, and training
are deferred, so Python mirrors only schema validation + migration, letting
pytest cross-check native behavior (§7.3).

Run tests from the repo root: `python3 -m pytest tests/python_tests -q`
