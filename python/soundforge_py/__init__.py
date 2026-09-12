# SoundForge G0 — Python reference & tooling package (§2.5).
"""SoundForge Python reference package (G0 stubs).

The C++ engine under ``native/`` is authoritative; this package hosts the
schema-validation wrapper, the Python mirror of the C++ migrator, and
placeholder namespaces for future import/reference/regression tooling.
"""

__version__ = "0.1.0-g1"

# Current canonical schema version (mirrors SF_SCHEMA_VERSION, sf_version.h).
SCHEMA_VERSION = 2
