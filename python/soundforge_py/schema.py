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

    Performs structural Draft 2020-12 validation (canonical schema) plus the
    semantic signal-graph checks that JSON Schema cannot express: dangling
    edge references and directed cycles (mirrors native schema.cpp check_graph).

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

    # Semantic graph checks (dangling references + cycles) — same rules as
    # check_graph() in native/src/core/schema.cpp.
    for key in ("signalGraph", "powerGraph"):
        graph = data.get(key)
        if not isinstance(graph, dict):
            continue
        nodes = graph.get("nodes")
        edges = graph.get("edges")
        if not isinstance(nodes, list) or not isinstance(edges, list):
            continue
        node_ids = set()
        for i, node in enumerate(nodes):
            if isinstance(node, dict) and isinstance(node.get("id"), str):
                node_ids.add(node["id"])
        adjacency: dict[str, list[str]] = {}
        for i, edge in enumerate(edges):
            if not isinstance(edge, dict):
                continue
            frm, to = edge.get("from"), edge.get("to")
            if isinstance(frm, str) and frm not in node_ids:
                errors.append(f"{key}/edges/{i}: 'from' references unknown node '{frm}'")
            if isinstance(to, str) and to not in node_ids:
                errors.append(f"{key}/edges/{i}: 'to' references unknown node '{to}'")
            if isinstance(frm, str) and isinstance(to, str):
                adjacency.setdefault(frm, []).append(to)

        # Directed-cycle detection (iterative DFS, three-color).
        color = {nid: 0 for nid in node_ids}  # 0=unvisited 1=in-progress 2=done
        for start in node_ids:
            if color[start] != 0:
                continue
            stack: list[tuple[str, int]] = [(start, 0)]  # (node, next-child-index)
            color[start] = 1
            while stack:
                u, idx = stack[-1]
                if idx < len(adjacency.get(u, [])):
                    stack[-1] = (u, idx + 1)
                    v = adjacency[u][idx]
                    if color.get(v) == 1:
                        errors.append(f"{key}: cycle detected in edges")
                        stack.clear()
                        break
                    if color.get(v) == 0:
                        color[v] = 1
                        stack.append((v, 0))
                else:
                    color[u] = 2
                    stack.pop()
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
