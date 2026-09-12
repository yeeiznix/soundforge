# SoundForge G1 — Python schema/migration tests (docs/PLAN_G0.md §7.3).
"""Pytest suite sharing the native test fixtures (tests/fixtures/).

Fixture paths are resolved relative to this test file so the suite runs from
any working directory: tests/python_tests/test_schema_py.py -> ../fixtures.
"""

import json
import sys
from pathlib import Path

_TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = _TEST_DIR.parents[1]
FIXTURES = _TEST_DIR.parent / "fixtures"

# Make the source package importable without installing it.
sys.path.insert(0, str(REPO_ROOT / "python"))

from soundforge_py import SCHEMA_VERSION  # noqa: E402
from soundforge_py.migrate import migrate_json  # noqa: E402
from soundforge_py.schema import validate_project  # noqa: E402


def _load(name: str):
    with open(FIXTURES / name, "r", encoding="utf-8") as fh:
        return json.load(fh)


def test_validate_minimal_v2():
    assert validate_project(_load("project_minimal_v2.json")) == []


def test_validate_corrupt_fails():
    assert len(validate_project(_load("project_corrupt.json"))) > 0


def test_migrate_v0_to_v2():
    data = _load("project_minimal_v0.json")
    migrate_json(data, 0, 2)
    assert data["schemaVersion"] == 2
    assert validate_project(data) == []


def test_migrate_v1_to_v2():
    data = _load("project_minimal_v1.json")
    migrate_json(data, 1, 2)
    assert data["schemaVersion"] == 2
    assert data["venue"]["dimensions"] == {"widthM": 12.0, "depthM": 10.0, "heightM": 4.0}
    assert data["scene"]["geometry"]["center"] == {"x": 6.0, "y": 5.0, "z": 2.0}
    assert validate_project(data) == []


def test_schema_version_const():
    assert SCHEMA_VERSION == 2