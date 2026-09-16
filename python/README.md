# SoundForge Python Layer — Reference/Regression Parity (G6)

## Overview

This layer provides host-side Python access to the native C++ SoundForge engine via a stdlib-only ctypes binding. It includes:

1. **Stdlib ctypes loader** (`soundforge_py/engine.py`): wraps ~39 C-ABI symbols from `libsoundforge.so`
2. **Pure-Python reference engine** (`reference/engine.py`): minimal-but-honest chain-gain renderer for parity validation (D5 scope: chain-only, no NumPy)
3. **Regression harness** (`regression/engine_parity.py`): single source of truth for parity tolerances, signal generators, and compare helpers
4. **Lockstep parity pytest suite** (`tests/python_tests/test_engine_parity.py`): deterministic audio parity (version/schema/migrate/validate/project round-trip + audio tick/meter/block)

## Loader Contract

The ctypes loader uses an env-controlled path resolution (test-only trust boundary):

```
$SOUNDFORGE_LIB_PATH (env)
  → absolute path, realpath-canonicalized, isfile-checked
  → non-absolute values rejected with RuntimeError
  ↓
REPO_ROOT/native/build/libsoundforge.so (default, reg build)
  ↓
RuntimeError (names both candidate paths, suggests cmake --build)
```

The `.so` is opened with explicit `RTLD_LOCAL` (never `RTLD_GLOBAL`). The internal C++ symbol table cannot interpose on or be interposed by other DSOs.

## Running the Parity Suite

### Default (reg build)

```bash
cd /root/project/soundforge
pytest tests/python_tests/ -v
```

Expects `native/build/libsoundforge.so` to exist (built by `cmake --build native/build`).

### UBSan build (hardened)

```bash
SOUNDFORGE_LIB_PATH=/root/project/soundforge/native/build-asan/libsoundforge.so \
  pytest tests/python_tests/ -v
```

Both invocations run the same test suite against their respective `.so` build artifacts, verifying deterministic parity across sanitizer configurations.

## Lifecycle & Memory

### AudioEngine Context Manager

```python
with Project.from_json(raw) as p:
    with CommandQueue.create() as q:
        with AudioEngine.create(q, p) as e:
            e.configure(...)
            e.set_output(...)
            e.start(0)  # deterministic ticks (no pacer)
            e.tick(frames)
            e.stop()
            e.join()
            meters = json.loads(e.meter_json())
```

**Ownership & destruction (reverse-create order, SEC-G6-06):**
- Create order: project → queue → engine
- Destroy order (on `__exit__`): engine → queue → project
- `AudioEngine` holds strong references to both; destruction is explicit via context managers (no `__del__` / finalizers)

### Callback Contract (SEC-G6-05)

io callbacks (read/write) run on the caller's thread (no GIL safety yet — future PACE gate):

- **Copy within the call:** pointers never retained or dereferenced after return
- **Channels == 2 tripwire:** `assert channels == 2` at entry (reject if not stereo)
- **try/except + error propagation:** exception → record + return `SF_E_IO` (never 0 == `SF_OK`, never let escape to ctypes)
- **Data type:** native reads/writes float32 pointers; Python callbacks receive `c_float` arrays

## Reference Engine Scope (D5)

The pure-Python reference computes deterministic chain-gain surfaces **only**:

- **In scope:** signal-graph topo order, per-node linear gain (`10^(gainDb/20)`), gain applied in topo order, DC meter (max abs = amplitude exactly)
- **Out of scope:** TruePeak 4× oversample FIR, pan, solo, mixer routing, multi-source graphs, NumPy

**Boundaries enforced (ORC-G6-P2-02/03/04):**
- Non-terminal `out_node_id` → `ValueError` (chain-only boundary)
- Malformed graphs (missing fields, invalid edges) → `ValueError` (guard with `.get()`/`isinstance`)
- Out-of-scope surfaces (`solo == true`, `pan != 0`) → `ValueError` ("out of D5 chain-gain scope")

## Parity Tolerances (ORC-G6-10)

Single source of truth in `regression/engine_parity.py`:

- **Meter (DC only):** `|native − reference| ≤ 1e-3` (mirrors unit tests; sine is block-parity only)
- **DC block samples:** `≤ 1e-6` absolute (mirrors `test_audio_engine.cpp:941-942`)
- **Sine block samples:** `≤ 1e-5` relative (`1e-5 × (1 + |ref|)` — float64 reference vs float32 engine)

## Fixtures & Validation

Fixtures are in `tests/fixtures/`:

- `project_minimal_v{0,1,2}.json` — minimal valid projects (v0/v1 are migrated to v2 for audio tests)
- `project_signalgraph_v2.json` — signal-graph example (2-node chain)
- `project_dspchain_v2.json` — excluded from round-trip (native codec lossy on `signalGraph.mixers` — ORC-G6-07)
- `project_corrupt.json`, `project_graph_corrupt.json` — validation rejection cases

Byte-identity rule (SEC-G6-07): fixtures are fed as raw bytes to the native side; the reference parses `json.loads(raw_bytes)` of the same serialization. No re-dump of a parsed dict ever reaches the native side.

## Test Coverage

P3 matrix expansion (docs/PLAN_G6.md §5 P3, §6.2):

- **P2 baseline:** 12 cases (version/schema lockstep, validate decision, migrate, round-trip, non-ASCII, lazy load, D4 anchors, silence, reset_meters, sine, byte identity)
- **P3 matrix:** 8 chains (0,0) / (-6,0) / (-6,+6) / (+6,0) × 2 block-sizes (64, 256) + save/open round-trip + mono-stereo agreement + negative cases (ORC-G6-P2-02/03/04)
- **Total:** ~26–30 cases (count grows with evidence; DoD records exact count post-P3)

All cases run on both reg and UBSan builds (same binary, two sanitizer configs). No fixed sleeps, no wall clock — tick is deterministic.

---

For detailed specification, see `docs/PLAN_G6.md` §3 (decisions D1–D6), §5 (phases P1–P4), §6.2 (pytest DoD).
