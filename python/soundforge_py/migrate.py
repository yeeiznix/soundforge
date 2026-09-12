# SoundForge G0 — Python mirror of the C++ v0→1 migrator (§5.5, §7.3).
"""Mirror of ``sfcore::migrate_doc_inplace`` (native/src/core/migration.cpp).

G0 path: 0→1, idempotent, injects defaults for missing canonical keys,
ensures envelope shape, appends a ``project.migrate`` audit entry. Key names
and default values MUST stay in lockstep with the C++ implementation.
"""

from __future__ import annotations

import uuid as _uuid
from datetime import datetime, timezone

# Mirrors sf_engine_version() / SF_VERSION_STRING (sf_version.h): the engine
# version stamped into documents that this code migrates.
ENGINE_VERSION = "0.1.0-g0"

# (collection key, envelope type) — mirrors collections() in sf_internal.hpp.
_COLLECTIONS: list[tuple[str, str]] = [
    ("audienceReceivers", "audienceReceiver"),
    ("equipment", "equipment"),
    ("audioAssets", "audioAsset"),
    ("dspPresets", "dspPreset"),
    ("arrayConfigurations", "arrayConfiguration"),
    ("measurements", "measurement"),
    ("simulationRuns", "simulationRun"),
    ("trainingScenarios", "trainingScenario"),
    ("inventoryRefs", "inventoryRef"),
    ("reports", "report"),
]

# Canonical closed key set (§9) — mirrors top_level_keys() in sf_internal.hpp.
_TOP_LEVEL_KEYS: list[str] = [
    "schemaVersion",
    "engineVersion",
    "project",
    "venue",
    "scene",
    "audienceReceivers",
    "equipment",
    "signalGraph",
    "powerGraph",
    "audioAssets",
    "dspPresets",
    "arrayConfigurations",
    "measurements",
    "simulationRuns",
    "trainingScenarios",
    "inventoryRefs",
    "reports",
    "auditLog",
]

_HEX = "0123456789abcdefABCDEF"


def _uuid_generate() -> str:
    return str(_uuid.uuid4())


def _now_iso8601() -> str:
    # Mirrors now_iso8601() (uuid.cpp): "%Y-%m-%dT%H:%M:%S.%03dZ", UTC.
    t = datetime.now(timezone.utc)
    return t.strftime("%Y-%m-%dT%H:%M:%S.") + f"{t.microsecond // 1000:03d}Z"


def _is_uuid(s: str) -> bool:
    # Mirrors is_uuid() (sf_internal.hpp): lower/upper hex v4 UUID.
    if not isinstance(s, str) or len(s) != 36:
        return False
    for i, c in enumerate(s):
        if i in (8, 13, 18, 23):
            if c != "-":
                return False
        elif c not in _HEX:
            return False
    return s[14] == "4" and s[19] in "89ab"  # version + variant nibbles


def _is_iso_timestamp(s: str) -> bool:
    # Mirrors is_iso_timestamp() (sf_internal.hpp): RFC3339 with ms or offset.
    if not isinstance(s, str) or len(s) < 20:
        return False

    def digits(off: int, n: int) -> bool:
        return all(s[off + k].isdigit() for k in range(n))

    if not (
        digits(0, 4)
        and s[4] == "-"
        and digits(5, 2)
        and s[7] == "-"
        and digits(8, 2)
        and s[10] == "T"
        and digits(11, 2)
        and s[13] == ":"
        and digits(14, 2)
        and s[16] == ":"
        and digits(17, 2)
    ):
        return False
    if not (1 <= int(s[5:7]) <= 12) or not (1 <= int(s[8:10]) <= 31):
        return False
    if int(s[11:13]) > 23 or int(s[14:16]) > 59 or int(s[17:19]) > 60:
        return False
    p = 19
    if p < len(s) and s[p] == ".":
        f = p
        p += 1
        while p < len(s) and s[p].isdigit():
            p += 1
        if p == f + 1:
            return False
    if p >= len(s):
        return False
    if s[p] == "Z":
        return p + 1 == len(s)
    if s[p] in "+-":
        if len(s) != p + 6:
            return False
        return digits(p + 1, 2) and s[p + 3] == ":" and digits(p + 4, 2)
    return False


def _peek_schema_version(j: dict) -> int:
    # Mirrors peek_schema_version() (json_codec.cpp): -1 if unknown/invalid.
    if isinstance(j, dict):
        v = j.get("schemaVersion")
        if isinstance(v, int) and not isinstance(v, bool):
            return v
    return -1


def _default_value_for_key(key: str):
    # Mirrors default_value_for_key() (migration.cpp).
    if key in ("signalGraph", "powerGraph"):
        return {"nodes": [], "edges": []}
    if key == "venue":
        return {"id": _uuid_generate(), "name": "Untitled Venue"}
    if key == "scene":
        return {
            "id": _uuid_generate(),
            "name": "Default Scene",
            "venueRef": "",  # wired to venue below
        }
    return []  # collections + auditLog


def _singular_for(key: str) -> str:
    for ckey, singular in _COLLECTIONS:
        if key == ckey:
            return singular
    return "equipment"


def migrate_json(data: dict, from_ver: int, to_ver: int) -> dict:
    """Migrate a project document in place (and return it).

    Mirrors ``migrate_doc_inplace``. Idempotent when the document is already
    at the target version. Raises ``ValueError`` for non-object roots,
    downgrades, and unsupported version paths.
    """
    if not isinstance(data, dict):
        raise ValueError("root: expected JSON object")
    if from_ver == to_ver:
        return data  # no-op
    if to_ver < from_ver:
        raise ValueError("downgrade not supported")

    if from_ver == 0 and to_ver == 1:
        if _peek_schema_version(data) >= 1:
            return data  # idempotent — already migrated

        data["schemaVersion"] = 1
        data["engineVersion"] = ENGINE_VERSION  # engine that performed migration

        # Project placeholder if absent/broken.
        project = data.get("project")
        if not isinstance(project, dict):
            now = _now_iso8601()
            project = {
                "id": _uuid_generate(),
                "name": "Migrated Project",
                "createdAt": now,
                "modifiedAt": now,
                "author": "",
                "notes": "",
            }
            data["project"] = project
        else:
            if not _is_uuid(project.get("id", "")):
                project["id"] = _uuid_generate()
            name = project.get("name")
            if not isinstance(name, str) or not name:
                project["name"] = "Migrated Project"
            for ts_key in ("createdAt", "modifiedAt"):
                ts = project.get(ts_key)
                if not (isinstance(ts, str) and _is_iso_timestamp(ts)):
                    project[ts_key] = _now_iso8601()
            for s in ("author", "notes"):
                if not isinstance(project.get(s), str):
                    project[s] = ""

        # Inject missing canonical keys.
        for key in _TOP_LEVEL_KEYS:
            if key not in data:
                data[key] = _default_value_for_key(key)

        # Ensure graph shape.
        for gk in ("signalGraph", "powerGraph"):
            g = data[gk]
            if not isinstance(g, dict):
                g = {"nodes": [], "edges": []}
                data[gk] = g
            if not isinstance(g.get("nodes"), list):
                g["nodes"] = []
            if not isinstance(g.get("edges"), list):
                g["edges"] = []

        # Ensure envelope shape for every collection element.
        for ckey, _singular in _COLLECTIONS:
            arr = data[ckey]
            if not isinstance(arr, list):
                arr = []
                data[ckey] = arr
            for idx, envelope in enumerate(arr):
                if not isinstance(envelope, dict):
                    envelope = {}
                    arr[idx] = envelope
                if not _is_uuid(envelope.get("id", "")):
                    envelope["id"] = _uuid_generate()
                etype = envelope.get("type")
                if not isinstance(etype, str) or not etype:
                    envelope["type"] = _singular_for(ckey)
                ver = envelope.get("version")
                if not isinstance(ver, int) or isinstance(ver, bool) or ver < 1:
                    envelope["version"] = 1
                for ts_key in ("createdAt", "modifiedAt"):
                    ts = envelope.get(ts_key)
                    if not (isinstance(ts, str) and _is_iso_timestamp(ts)):
                        envelope[ts_key] = _now_iso8601()
                prov = envelope.get("provenance")
                if not isinstance(prov, str) or not prov:
                    envelope["provenance"] = "migrated:v0->v1"
                if not isinstance(envelope.get("data"), dict):
                    envelope["data"] = {}

        # Wire scene.venueRef to venue id.
        venue = data["venue"]
        if not isinstance(venue, dict):
            raise ValueError("venue: expected JSON object")
        if not _is_uuid(venue.get("id", "")):
            venue["id"] = _uuid_generate()
        scene = data["scene"]
        if not isinstance(scene, dict):
            raise ValueError("scene: expected JSON object")
        if not _is_uuid(scene.get("id", "")):
            scene["id"] = _uuid_generate()
        if not _is_uuid(scene.get("venueRef", "")):
            scene["venueRef"] = venue["id"]

        # Audit entry.
        entry = {
            "ts": _now_iso8601(),
            "actor": "system",
            "action": "project.migrate",
            "objectId": data["project"]["id"],
            "detail": "migrated 0->1",
        }
        if not isinstance(data["auditLog"], list):
            data["auditLog"] = []
        data["auditLog"].append(entry)

        return data

    raise ValueError(f"no migration path from {from_ver} to {to_ver}")
