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


def test_golden_v1_immutable():
    # G1 keeps schema_golden_v1.json beside v2 (PLAN_G1 Appendix B (a)) but the
    # drift DoD only diffs the v2 schema, so nothing else verifies v1 stays
    # frozen. Pin its SHA-256 here: any regeneration or edit of the v1 golden
    # must be an explicit, reviewed change.
    import hashlib

    path = REPO_ROOT / "tests" / "golden" / "schema_golden_v1.json"
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    assert digest == "2a2b1ca474d5b693b74cf6299188699f1e4d5a69e3f93f03ccf4a7a4b544a57f"


def test_golden_v2_immutable():
    import hashlib
    path = REPO_ROOT / "tests" / "golden" / "schema_golden_v2.json"
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    # SHA-256 computed from the regenerated file (byte-identical to project_schema.json)
    assert digest == "48bd5b70b001ca79c7b5c3ead1aab523ceb6964c622568d7c6d65772a29e627e"


def test_validate_signalgraph_v2():
    assert validate_project(_load("project_signalgraph_v2.json")) == []


def test_validate_graph_corrupt():
    errors = validate_project(_load("project_graph_corrupt.json"))
    # 4 errors: invalid node kind, mixer missing nodeId, dangling edge, cycle.
    assert len(errors) >= 4, errors