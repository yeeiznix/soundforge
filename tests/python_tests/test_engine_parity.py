# SoundForge G6 — P1 lockstep + P2 audio parity subset
# (docs/PLAN_G6.md §5 P1 + P2, §6.2, §3.4 D4, §3.5 D5).
"""ctypes parity: version/schema lockstep, validate decision, migrate
normalized equality, project round-trip + getters, non-ASCII, lazy load
(P1), and the audio engine surface: tick/meter/block parity against the
pure-Python reference engine (P2).

Authority: ``libsoundforge.so`` (the real C++ engine) via ctypes; the Python
mirror (schema.py / migrate.py) and the D5 reference engine are the
expectation. Raw fixture bytes feed the native entry points (byte-identity
rule, SEC-G6-07), and audio parity serializes each chain document **once** as
Python bytes — the exact same bytes go to ``sf_project_from_json`` and to
``json.loads`` for the reference (no re-dump for the native side).
Deterministic — no fixed sleeps, no wall clock (start(0), no pacer).
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
    AudioEngine,
    CommandQueue,
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

# P2 layer: reference engine + parity harness (tolerances live there ONLY —
# ORC-G6-10).
from reference.engine import ReferenceEngine  # noqa: E402
from regression.engine_parity import (  # noqa: E402
    BlockCapture,
    TOL_DC_BLOCK_ABS,
    TOL_METER_DC,
    compare_blocks,
    compare_meter,
    dc_source,
    sine_source,
)


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


# ===========================================================================
# P2 — audio engine: tick/meter/block parity vs the pure-Python reference
# (PLAN_G6.md §3.4 D4, §3.5 D5, §5 P2). Each case = one fresh engine session
# (G5/F doctrine — the engine/runner are one-shot; no engine shared over
# cases). Byte identity (SEC-G6-07): the chain document is serialized ONCE as
# Python bytes; the same bytes feed sf_project_from_json and json.loads for
# the reference. No fixed sleeps — tick is deterministic (start(0), no pacer).
# ===========================================================================

SRC_ID = "a10e8400-e29b-41d4-a716-446655440001"
OUT_ID = "a10e8400-e29b-41d4-a716-446655440002"

SAMPLE_RATE = 48000
CHANNELS = 2
BLOCK = 256  # ≤ 512 (kBlockMaxSamples) — required by the tick contract


def _chain_doc(src_gain_db: float, out_gain_db: float = 0.0) -> dict:
    """Fixed-gain chain src(-6dB)->out(+6dB)... built as **project JSON in
    Python** (the reference consumes the same document)."""
    return {
        "schemaVersion": 2,
        "engineVersion": "0.1.0-g1",
        "project": {
            "id": "550e8400-e29b-41d4-a716-446655440000",
            "name": "P2 Chain",
            "createdAt": "2026-09-11T00:00:00.000Z",
            "modifiedAt": "2026-09-11T00:00:00.000Z",
            "author": "",
            "notes": "",
        },
        "venue": {
            "id": "660e8400-e29b-41d4-a716-446655440000",
            "name": "Untitled Venue",
            "dimensions": {"widthM": 12.0, "depthM": 10.0, "heightM": 4.0},
        },
        "scene": {
            "id": "770e8400-e29b-41d4-a716-446655440000",
            "name": "Default Scene",
            "venueRef": "660e8400-e29b-41d4-a716-446655440000",
            "geometry": {
                "center": {"x": 6.0, "y": 5.0, "z": 2.0},
                "listening": {"x": 6.0, "y": 5.0, "z": 2.0},
            },
        },
        "audienceReceivers": [],
        "equipment": [],
        "signalGraph": {
            "nodes": [
                {
                    "kind": "source",
                    "id": SRC_ID,
                    "label": "src",
                    "position": {"x": 0.0, "y": 0.0},
                    "mixer": {
                        "mute": False,
                        "solo": False,
                        "gainDb": src_gain_db,
                    },
                },
                {
                    "kind": "output",
                    "id": OUT_ID,
                    "label": "out",
                    "position": {"x": 100.0, "y": 0.0},
                    "mixer": {
                        "mute": False,
                        "solo": False,
                        "gainDb": out_gain_db,
                    },
                },
            ],
            "edges": [
                {"from": SRC_ID, "to": OUT_ID, "label": "Signal Path"}
            ],
        },
        "powerGraph": {"nodes": [], "edges": []},
        "audioAssets": [],
        "dspPresets": [],
        "arrayConfigurations": [],
        "measurements": [],
        "simulationRuns": [],
        "trainingScenarios": [],
        "inventoryRefs": [],
        "reports": [],
        "auditLog": [],
    }


def _run_engine(
    raw: bytes,
    *,
    out_node_id: str = OUT_ID,
    read_cb=None,
    write_cb=None,
    ticks=(BLOCK,),
    max_block: int = BLOCK,
):
    """One fresh engine session per case:
    create → configure → set_output → start(0) → tick* → stop → join →
    destroy. Queue + project outlive the engine; AudioEngine.__exit__
    destroys in reverse (engine → queue → project, SEC-G6-06).

    Returns the meter dict read before teardown.
    """
    with Project.from_json(raw) as p:  # noqa: F841 — outlives engine
        with CommandQueue.create() as q:  # noqa: F841 — outlives engine
            with AudioEngine.create(q, p) as e:
                e.configure(
                    sample_rate=SAMPLE_RATE,
                    channels=CHANNELS,
                    max_block_frames=max_block,
                    read_cb=read_cb,
                    write_cb=write_cb,
                )
                e.set_output(out_node_id)
                e.start(0)  # no pacer — deterministic ticks
                for frames in ticks:
                    e.tick(frames)
                e.stop()
                e.join()
                meters = json.loads(e.meter_json())
    return meters


# --- D4 anchors: fixed-gain DC chains -------------------------------------

# (src_db, out_db) → exact anchor; expected output = 10^(src/20) × 10^(out/20)
# (per-node gain applied in topo order; unity DC input). clipped mirrors the
# unit tests: true only when the true peak reaches 1.0 (D3, 0 dBFS contract).
_ANCHOR_CASES = [
    pytest.param(-6.0, 0.0, 10.0 ** (-6.0 / 20.0), False, id="minus6-zero"),   # 0.501187
    pytest.param(-6.0, 6.0, 10.0 ** (-6.0 / 20.0) * 10.0 ** (6.0 / 20.0), False, id="minus6-plus6"),  # 1.0
    pytest.param(6.0, 0.0, 10.0 ** (6.0 / 20.0), True, id="plus6-zero"),       # 1.995262
]


@pytest.mark.parametrize("src_db,out_db,anchor,clipped", _ANCHOR_CASES)
def test_dc_anchor_chain_meter_and_block(
    soundforge_lib, src_db: float, out_db: float, anchor: float, clipped: bool
):
    """D4 anchors: meter + block vs the exact DC value.

    chain `src(src_db dB)->out(out_db dB)`, output OUT_ID:
    - meter |native − reference| ≤ 1e-3 (TOL_METER_DC);
    - DC block samples ≤ 1e-6 absolute (TOL_DC_BLOCK_ABS);
    - clipped == True only for (+6,0) (0 dBFS contract, D3).

    Meter anchor is read AFTER the prime→reset→steady cycle (G5 fixture math,
    test_audio_engine.cpp:931-945): 8 ticks warm the true-peak FIR tail,
    reset_meters zeroes the latches (tails kept), one more tick is steady
    state — the FIR cold-start onset inflation is not part of the anchor.
    Reference renders lockstep (8 + 1 = 9 on both sides).
    """
    doc = _chain_doc(src_db, out_db)
    raw = json.dumps(doc).encode("utf-8")  # byte identity (SEC-G6-07)

    # Native session: DC source fills L/R; capture the rendered blocks.
    src = dc_source(1.0)
    cap = BlockCapture()
    ref = ReferenceEngine()
    ref_src = dc_source(1.0)
    ref_doc = json.loads(raw)

    with Project.from_json(raw) as p:  # noqa: F841 — outlives engine
        with CommandQueue.create() as q:  # noqa: F841 — outlives engine
            with AudioEngine.create(q, p) as e:
                e.configure(
                    sample_rate=SAMPLE_RATE,
                    channels=CHANNELS,
                    max_block_frames=BLOCK,
                    read_cb=src.cb,
                    write_cb=cap.cb,
                )
                e.set_output(OUT_ID)
                e.start(0)  # no pacer — deterministic ticks

                # Prime the meter tail (lockstep with the reference).
                for _ in range(8):
                    e.tick(BLOCK)
                    ref.render(ref_doc, OUT_ID, ref_src, BLOCK)
                # Latches zeroed, tails kept → next tick is steady state.
                e.reset_meters()
                e.tick(BLOCK)
                ref_block, (ref_m_l, ref_m_r) = ref.render(
                    ref_doc, OUT_ID, ref_src, BLOCK
                )

                e.stop()
                e.join()
                meters = json.loads(e.meter_json())

    # Lockstep block count (8 prime + 1 steady tick / render).
    assert meters["blocksRendered"] == ref.blocks_rendered == 9

    # Meter parity (DC only) + anchor pin.
    native_lin = meters["truePeakLinear"]
    assert native_lin[0] is not None and native_lin[1] is not None  # NOT null linear
    compare_meter(native_lin[0], ref_m_l, kind="dc")
    compare_meter(native_lin[1], ref_m_r, kind="dc")
    assert native_lin[0] == pytest.approx(anchor, abs=TOL_METER_DC)
    assert native_lin[1] == pytest.approx(anchor, abs=TOL_METER_DC)

    # Block parity: final captured block vs reference block (≤1e-6 absolute).
    assert len(cap.blocks) == 9
    captured_l, captured_r = cap.blocks[-1]
    compare_blocks(captured_l, ref_block, kind="dc")
    compare_blocks(captured_r, ref_block, kind="dc")
    # Anchor on the block samples directly (10^(-6/20) etc.).
    assert captured_l[0] == pytest.approx(anchor, abs=TOL_DC_BLOCK_ABS)
    assert captured_r[0] == pytest.approx(anchor, abs=TOL_DC_BLOCK_ABS)

    # Clipping probe: (+6,0) → true, (-6,0)/( -6,+6) → false (D3 0 dBFS).
    assert meters["clipped"] == [clipped, clipped]

    # Callback contract teardown: ZERO recorded callback errors (SEC-G6-05).
    assert src.errors == [], f"read callback errors: {src.errors}"
    assert cap.errors == [], f"write callback errors: {cap.errors}"


def test_silence_io_null_meter(soundforge_lib):
    """Silence path (io NULL — no read/write callbacks): the frozen engine
    emits truePeakLinear == [0.0, 0.0] and truePeakDb == [null, null] for
    both channels (ORC-G6-05 — NOT null linear)."""
    doc = _chain_doc(0.0, 0.0)
    raw = json.dumps(doc).encode("utf-8")

    meters = _run_engine(raw, read_cb=None, write_cb=None)

    lin = meters["truePeakLinear"]
    db = meters["truePeakDb"]
    assert lin == [0.0, 0.0], f"expected [0.0, 0.0], got {lin}"
    assert db == [None, None], f"expected null dB, got {db}"
    assert meters["clipped"] == [False, False]

    # Reference side agrees: DC 0.0 → output block all zeros → meter 0.0.
    ref = ReferenceEngine()
    ref_src = dc_source(0.0)
    ref_block, (ref_m_l, ref_m_r) = ref.render(json.loads(raw), OUT_ID, ref_src, BLOCK)
    assert ref_block == [0.0] * BLOCK
    assert ref_m_l == ref_m_r == 0.0
    assert meters["blocksRendered"] == ref.blocks_rendered == 1


def test_reset_meters_keeps_blocks_rendered_monotonic(soundforge_lib):
    """reset_meters zeroes the latches but keeps blocksRendered (monotonic,
    NOT reset — lockstep with the reference's render counter)."""
    doc = _chain_doc(0.0, 0.0)
    raw = json.dumps(doc).encode("utf-8")
    src = dc_source(0.5)
    cap = BlockCapture()
    ref = ReferenceEngine()
    ref_src = dc_source(0.5)
    ref_doc = json.loads(raw)

    with Project.from_json(raw) as p:  # noqa: F841
        with CommandQueue.create() as q:  # noqa: F841
            with AudioEngine.create(q, p) as e:
                e.configure(
                    sample_rate=SAMPLE_RATE,
                    channels=CHANNELS,
                    max_block_frames=BLOCK,
                    read_cb=src.cb,
                    write_cb=cap.cb,
                )
                e.set_output(OUT_ID)
                e.start(0)

                # Prime the meter tail (G5 unit-test doctrine), lockstep with
                # the reference renders.
                for _ in range(8):
                    e.tick(128)
                    ref.render(ref_doc, OUT_ID, ref_src, 128)
                m1 = json.loads(e.meter_json())
                assert m1["blocksRendered"] == ref.blocks_rendered == 8
                # NOTE: no steady-value assert on m1 — the true-peak LATCH
                # holds the FIR's cold-start onset overshoot (~0.5 × 1.1259)
                # until reset; steady state is asserted after reset_meters
                # (mirrors ResetMetersZeroesLatchesButNotTails).

                # Latches zeroed, tails kept, blocksRendered NOT reset.
                m2 = json.loads(e.meter_json())
                e.reset_meters()
                m3 = json.loads(e.meter_json())
                assert m3["blocksRendered"] == 8  # monotonic — not reset
                assert m3["blocksRendered"] == ref.blocks_rendered  # lockstep
                assert m3["truePeakLinear"] == [0.0, 0.0]
                assert m3["truePeakDb"] == [None, None]

                # One more block is in steady state (tail preserved): ~0.5.
                e.tick(128)
                ref.render(ref_doc, OUT_ID, ref_src, 128)
                m4 = json.loads(e.meter_json())
                assert m4["blocksRendered"] == ref.blocks_rendered == 9
                assert m4["truePeakLinear"][0] == pytest.approx(0.5, abs=TOL_METER_DC)

                e.stop()
                e.join()
                last = json.loads(e.meter_json())
                assert last["blocksRendered"] == 9  # stop/join don't render

    assert src.errors == [], f"read callback errors: {src.errors}"
    assert cap.errors == [], f"write callback errors: {cap.errors}"


def test_sine_block_parity(soundforge_lib):
    """Sine (1 kHz @ 48 kHz, N=256) output-**block** parity ≤ 1e-5 relative
    (float64 reference vs float32 engine). Meter parity is DC-ONLY (D5/G6-1):
    no sine meter compare here."""
    doc = _chain_doc(0.0, 0.0)  # unity chain — pure sine passthrough
    raw = json.dumps(doc).encode("utf-8")

    src = sine_source(1000.0, SAMPLE_RATE)
    cap = BlockCapture()
    meters = _run_engine(raw, read_cb=src.cb, write_cb=cap.cb)

    # Reference: the SAME signal generator (a fresh sine_source instance
    # starts at the same phase 0 and produces the identical waveform).
    ref = ReferenceEngine()
    ref_src = sine_source(1000.0, SAMPLE_RATE)
    ref_block, (ref_m_l, ref_m_r) = ref.render(json.loads(raw), OUT_ID, ref_src, BLOCK)

    assert len(cap.blocks) == 1
    captured_l, captured_r = cap.blocks[-1]
    compare_blocks(captured_l, ref_block, kind="sine")
    compare_blocks(captured_r, ref_block, kind="sine")

    # Block parity only — meter is DC-only (D5/G6-1).
    assert meters["blocksRendered"] == ref.blocks_rendered == 1

    assert src.errors == [], f"read callback errors: {src.errors}"
    assert cap.errors == [], f"write callback errors: {cap.errors}"


def test_chain_byte_identity_native_input_is_raw_bytes(soundforge_lib):
    """Byte identity (SEC-G6-07) is real: the native side consumes the exact
    serialized bytes — the reference parses the SAME bytes. No re-dump of a
    parsed dict ever reaches sf_project_from_json."""
    doc = _chain_doc(-6.0, 6.0)
    raw = json.dumps(doc).encode("utf-8")

    # The anchor case relies on this; prove it here explicitly: the native
    # project parses from `raw`, and json.loads(raw) is the reference doc.
    ref_doc = json.loads(raw)
    assert ref_doc["signalGraph"]["nodes"][0]["mixer"]["gainDb"] == -6.0

    src = dc_source(1.0)
    cap = BlockCapture()
    ref = ReferenceEngine()
    ref_src = dc_source(1.0)

    with Project.from_json(raw) as p:  # noqa: F841
        with CommandQueue.create() as q:  # noqa: F841
            with AudioEngine.create(q, p) as e:
                e.configure(
                    sample_rate=SAMPLE_RATE,
                    channels=CHANNELS,
                    max_block_frames=BLOCK,
                    read_cb=src.cb,
                    write_cb=cap.cb,
                )
                e.set_output(OUT_ID)
                e.start(0)
                # Prime the meter tail, then one steady tick (anchor cycle).
                for _ in range(8):
                    e.tick(BLOCK)
                    ref.render(ref_doc, OUT_ID, ref_src, BLOCK)
                e.reset_meters()
                e.tick(BLOCK)
                ref_block, (ref_m_l, _) = ref.render(
                    ref_doc, OUT_ID, ref_src, BLOCK
                )
                e.stop()
                e.join()
                meters = json.loads(e.meter_json())

    # ((-6, +6) unity product) — engine renders 1.0 through both nodes
    # (block exact on every tick; meter steady after the prime cycle).
    assert len(cap.blocks) == 9
    captured_l, captured_r = cap.blocks[-1]
    assert captured_l[0] == pytest.approx(1.0, abs=TOL_DC_BLOCK_ABS)
    assert captured_r[0] == pytest.approx(1.0, abs=TOL_DC_BLOCK_ABS)
    compare_blocks(captured_l, ref_block, kind="dc")
    assert meters["truePeakLinear"][0] == pytest.approx(1.0, abs=TOL_METER_DC)
    compare_meter(meters["truePeakLinear"][0], ref_m_l, kind="dc")
    assert meters["blocksRendered"] == ref.blocks_rendered == 9
    assert src.errors == [] and cap.errors == []