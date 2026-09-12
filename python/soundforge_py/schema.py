# SoundForge G0 — schema validation wrapper (§2.5, §3.3, §7.3).
"""Load the canonical project JSON Schema and validate project documents.

``native/data/schemas/project_schema.json`` is the single source of truth;
this module only reads it (Draft 2020-12 via ``jsonschema``).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

from jsonschema import Draft202012Validator

# Repo root, resolved from this file: python/soundforge_py/schema.py
REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "native" / "data" / "schemas" / "project_schema.json"


def load_schema(path: Path | str | None = None) -> dict:
    """Load the canonical project schema (Draft 2020-12)."""
    schema_path = Path(path) if path is not None else SCHEMA_PATH
    with open(schema_path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def validate_project(data: dict, schema: dict | None = None) -> list[str]:
    """Validate a project document.

    Returns a list of human-readable error strings; an empty list means the
    document is valid. All errors are collected (not fail-fast).
    """
    schema = schema if schema is not None else load_schema()
    validator = Draft202012Validator(
        schema, format_checker=Draft202012Validator.FORMAT_CHECKER
    )
    errors: list[str] = []
    for err in sorted(validator.iter_errors(data), key=lambda e: list(e.absolute_path)):
        where = "/".join(str(p) for p in err.absolute_path) or "<root>"
        errors.append(f"{where}: {err.message}")
    return errors


def _main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: python3 python/soundforge_py/schema.py <project.json>", file=sys.stderr)
        return 2
    try:
        with open(argv[1], "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    errs = validate_project(doc)
    if errs:
        print("INVALID")
        for e in errs:
            print(f"  - {e}")
        return 1
    print("VALID")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv))
