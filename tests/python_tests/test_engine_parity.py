# SoundForge G6 — P1 lockstep parity subset (docs/PLAN_G6.md §5 P1, §6.2).
"""ctypes parity: version/schema lockstep, validate decision, migrate
normalized equality, project round-trip + getters, non-ASCII, lazy load.

Authority: ``libsoundforge.so`` (the real C++ engine) via ctypes; the Python
mirror (schema.py / migrate.py) is the expectation. Raw fixture bytes feed the
native entry points (byte-identity rule, SEC-G6-07). Deterministic — no fixed
sleeps, no wall clock.
"""

from __future__ import annotations

import importlib
import json
import re
import sys
from pathlib import Path

import pytest

_TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = _TEST_DIR.parents[1]
FIXTURES = _TEST_DIR.parent / "fixtures"

# Make the source package importable without installing it (same as
# test_schema_py.py).
sys.path.insert(0, str(REPO_ROOT / "python"))

from soundforge_py import SCHEMA_VERSION, __version__  # noqa: E402
from soundforge_py.engine import (  # noqa: E402
    SF_OK,
    Project,
    engine_version,
    get_lib,
    is_compatible,
    migrate_json as native_migrate_json,
    schema_version,
    validate_project_json,
)
from soundforge_py.migrate import migrate_json as py_migrate_json  # noqa: E402
from soundforge_py.schema import validate_project  # noqa: E402


def _fixture_bytes(name: str) -> bytes:
    return (FIXTURES / name).read_bytes()


# --- version / schema lockstep --------------------------------------------

def test_version_lockstep(soundforge_lib):
    """Engine stamp == package __version__ == 0.1.0-g5 (pre-P4); schema == 2;
    sf_is_compatible truth table {-1:0, 0:1, 1:1, 2:1, 3:0}."""
    assert engine_version() == __version__ == "0.1.0-g5"
    assert schema_version() == SCHEMA_VERSION == 2
    for v, expect in ((-1, 0), (0, 1), (1, 1), (2, 1), (3, 0)):
        assert bool(is_compatible(v)) is bool(expect), f"is_compatible({v})"


# --- validate decision parity (byte-identity, SEC-G6-07) -------------------

_VALIDATE_FIXTURES = [
    ("project_minimal_v2.json", True),
    ("project_signalgraph_v2.json", True),
    ("project_dspchain_v2.json", True),
    ("project_corrupt.json", False),
    ("project_graph_corrupt.json", False),
]


@pytest.mark.parametrize("fixture,expect_valid", _VALIDATE_FIXTURES)
def test_validate_decision_parity(soundforge_lib, fixture: str, expect_valid: bool):
    """Accept/reject decision + error presence equal: native vs Python."""
    raw = _fixture_bytes(fixture)  # raw file bytes to the native side
    code, message = validate_project_json(raw)
    native_ok = code == SF_OK
    errors = validate_project(json.loads(raw))
    py_ok = len(errors) == 0
    assert native_ok == expect_valid, f"native decision on {fixture}"
    assert py_ok == expect_valid, f"python decision on {fixture}"
    # error presence equal (messages are not compared — two validators by design)
    assert bool(message) == bool(errors), f"error presence on {fixture}"


# --- migrate normalized equality (ORC-G6-04) -------------------------------

_UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-4[0-9a-fA-F]{3}-"
    r"[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$"
)
_TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?(Z|[+-]\d{2}:\d{2})$")
_AUDIT_KEYS = ("ts", "actor", "action", "objectId", "detail")


def _mask(value):
    """Mask UUID-v4 and RFC3339 strings (both migrators inject fresh ones)."""
    if isinstance(value, str):
        if _UUID_RE.match(value):
            return "<uuid>"
        if _TS_RE.match(value):
            return "<ts>"
        return value
    if isinstance(value, list):
        return [_mask(v) for v in value]
    if isinstance(value, dict):
        return {k: _mask(v) for k, v in value.items()}
    return value


def _normalize_migrated(doc: dict) -> dict:
    """UUID/RFC3339 masked; auditLog as ordered tuples (masked ts);
    engineVersion kept strict (not masked) — the real lockstep signal."""
    out = _mask(doc)
    out["auditLog"] = [
        tuple(e.get(k) for k in _AUDIT_KEYS) for e in out.get("auditLog", [])
    ]
    return out


@pytest.mark.parametrize("from_ver", [0, 1])
def test_migrate_normalized_equality(soundforge_lib, from_ver: int):
    """v{from_ver}->2: native and Python migration produce normalized-equal docs."""
    raw = _fixture_bytes(f"project_minimal_v{from_ver}.json")

    # Native: in-place migration over the raw fixture bytes.
    code, out = native_migrate_json(raw, from_ver, 2)
    assert code == SF_OK, f"native migrate v{from_ver}->2 failed"
    native_doc = json.loads(out)

    # Python: in-place dict migration of the same bytes.
    py_doc = json.loads(raw)
    py_migrate_json(py_doc, from_ver, 2)

    assert _normalize_migrated(native_doc) == _normalize_migrated(py_doc)
    # engineVersion is kept strict inside _normalize_migrated; pin the anchor:
    assert native_doc["engineVersion"] == "0.1.0-g5"
    assert native_doc["schemaVersion"] == 2


# --- project round-trip + getters (ORC-G6-06/09) ---------------------------

_ROUNDTRIP_FIXTURES = ["project_minimal_v2.json", "project_signalgraph_v2.json"]
# dspchain_v2 EXCLUDED: native codec drops signalGraph.mixers — known-lossy
# (ORC-G6-07, residual G6-9).


@pytest.mark.parametrize("fixture", _ROUNDTRIP_FIXTURES)
def test_project_roundtrip_canonical(soundforge_lib, fixture: str):
    """Canonical-form idempotence: pass1 == pass2 == pass3 of
    to_json(from_json(X)). NOT raw fixture bytes (dspPresetRef:null /
    gainDb:0.0 emission differs per fixture — ORC-G6-06)."""
    raw = _fixture_bytes(fixture)
    with Project.from_json(raw) as p:
        pass1 = p.to_json()
    with Project.from_json(pass1.encode("utf-8")) as p:
        pass2 = p.to_json()
    with Project.from_json(pass2.encode("utf-8")) as p:
        pass3 = p.to_json()
    assert json.loads(pass1) == json.loads(pass2) == json.loads(pass3)


@pytest.mark.parametrize("fixture", _ROUNDTRIP_FIXTURES)
def test_project_getters_lockstep(soundforge_lib, fixture: str):
    """Getters vs fixture fields; engine_version vs the fixture's OWN
    engineVersion stamp (0.1.0-g1), never __version__ (ORC-G6-09)."""
    raw = _fixture_bytes(fixture)
    doc = json.loads(raw)
    with Project.from_json(raw) as p:
        assert p.name == doc["project"]["name"]
        assert p.id == doc["project"]["id"]
        assert p.schema_version == doc["schemaVersion"] == 2
        assert p.engine_version == doc["engineVersion"]  # e.g. 0.1.0-g1


# --- non-ASCII round-trip (SEC-G6-03 byte-length discipline) ---------------

def test_non_ascii_roundtrip(soundforge_lib):
    """'café' in venue/scene names round-trips and validates on both sides
    (proves the _as_bytes byte-length path, never char count)."""
    doc = json.loads(_fixture_bytes("project_minimal_v2.json"))
    doc["venue"]["name"] = "Café de la Scène"
    doc["scene"]["name"] = "Café Scène"
    raw = json.dumps(doc, ensure_ascii=False).encode("utf-8")
    assert "é" in raw.decode("utf-8")  # genuinely non-ASCII bytes on the wire

    # Validates on both sides.
    code, message = validate_project_json(raw)
    assert code == SF_OK, message
    assert validate_project(json.loads(raw)) == []

    # Round-trips through the native codec.
    with Project.from_json(raw) as p:
        assert p.name == doc["project"]["name"]
        out = p.to_json()
    back = json.loads(out)
    assert back["venue"]["name"] == "Café de la Scène"
    assert back["scene"]["name"] == "Café Scène"


# --- lazy load (D3 path contract; no .so required at import time) ---------

def test_lazy_load_runtime_error(monkeypatch):
    """Binding call without a usable .so raises the D3 RuntimeError: relative
    SOUNDFORGE_LIB_PATH rejected; absolute-but-missing raises naming
    candidates. Reload resets the singleton so the env is re-read (the env is
    read once per singleton creation)."""
    import soundforge_py.engine as engine_mod

    monkeypatch.setenv("SOUNDFORGE_LIB_PATH", "relative/libsoundforge.so")
    importlib.reload(engine_mod)
    with pytest.raises(RuntimeError):
        engine_mod.get_lib()

    monkeypatch.setenv("SOUNDFORGE_LIB_PATH", "/nonexistent/sf/libsoundforge.so")
    importlib.reload(engine_mod)
    with pytest.raises(RuntimeError):
        engine_mod.get_lib()