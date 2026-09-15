# SoundForge G0 — Python reference & tooling package (§2.5).
"""SoundForge Python reference package (G0 stubs).

The C++ engine under ``native/`` is authoritative; this package hosts the
schema-validation wrapper, the Python mirror of the C++ migrator, and
placeholder namespaces for future import/reference/regression tooling.
"""

# Gate history (PLAN_G3/G4 §5.3): the planned G2 "-g2" bump never landed; G3
# lands "-g3" directly; G4 lands "-g4".
__version__ = "0.1.0-g4"

# Current canonical schema version (mirrors SF_SCHEMA_VERSION, sf_version.h).
SCHEMA_VERSION = 2
