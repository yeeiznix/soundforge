# SoundForge — Gate G2 Plan: Signal Graph Data Model + Mixer/SignalEditor UI

> **Status:** DRAFT (authored post-g1-complete; scheduled for adversarial review by @planner/@oracle/@security-reviewer).  
> **Supersedes:** `docs/PLAN_G1.md` for G2 scope only. G0+G1 remain the contract for everything not changed here.  
> **Prior tags:** `g0-complete`, `g1-complete` on `main`.

---

## 1. Scope

### 1.1 Goal
The project document gains a **structural signal graph** (`signalGraph.nodes[]`, `signalGraph.edges[]`) with validation (acyclic, single output, no dangling edges) and native mutators to build/edit graphs. The Mixer and SignalEditor screens transition from G0/G1 placeholders to **list/form-based editors** that display nodes/edges and surface validation errors via the existing `UiState.Error`/`errorMessage` pattern. No real-time audio processing lands — evaluation is structural only (topology validity).

### 1.2 In Scope (G2 only)
- **Signal graph data model** (`native/src/graph/`, `sf_graph.h`):
  - Node types: `Input`, `Gain`, `EQ`, `Output` (minimal taxonomy; extensible via `params[4]`)
  - Edge semantics: `sourceNodeId`, `targetNodeId`, `sourcePort`, `targetPort`
  - Topology rules: acyclic (DFS cycle detection), single `Output` node, all edges connected to valid nodes
  - Structural validation: `sf_graph_validate(const SfProjectDoc*, errBuf)` — cycles, dangling edges, fan-out rules
  - Native mutators: `sf_graph_add_node`, `sf_graph_remove_node`, `sf_graph_connect`, `sf_graph_disconnect`, `sf_graph_set_node_params` — all with audit entries (`signalGraph.node.add` etc.) + `set_last_error` + `SF_*` codes
- **Mixer UI** (`app/ui/mixer/MixerScreen.kt`): Replace 24-LOC stub with list-based node editor (nodes list, edge connections display, add/remove controls)
- **SignalEditor UI** (`app/ui/signal/SignalEditorScreen.kt`): Replace stub with form-based graph editor (node params, edge visualization placeholder)
- **NativeBridge additions**: 4 new external funs (`addGraph_node`, `removeGraph_node`, `connectGraph_edge`, `disconnectGraph_edge`) + JNI exports
- **audioAssets sidecars**: Implement minimal `.sfasset` sidecar I/O (read/write binary blob, `sidecarPath` field in `audioAssets[].data`)
- **Python mirror sync**: Update `python/soundforge_py/schema.py` if schema changes (no version bump if possible)

### 1.3 Explicitly Out of Scope (G3+ — must stay stub/unchanged)
- **Real-time audio processing/rendering** — G3 (DSP chain execution)
- **Command queue implementation** — Explicitly **out of scope** (see Appendix A.1); audio-thread synchronization deferred to G3
- **New JNI exports beyond 4 G2 additions** — current count: 16 (G1); G2 adds 4 → expect **20**
- **Any `sf_*` ABI signature changes** — additive only
- **Power graph evaluation** — remains empty JSON object
- **Visual node graph editor** (drag-drop, bezier edges) — G4+; G2 uses list/form pattern only

### 1.4 Gating Rule
Same as G0/G1: files listed as *stub* are ≤20 LOC, reachable from `include/`, and carry `// G0: stub — G1+ implements` (unchanged for G3+ stubs). Files listed as *new* implement their bounded contract only. No file >300 LOC (advisory).

---

## 2. Architecture Changes

```
                    ┌──────────────────────────────────────────────┐
    Kotlin UI         │  MixerScreen.kt  SignalEditorScreen.kt    │
                     │  ProjectViewModel.kt (+graph mutators)     │
                     └───────────────┬──────────────────────────────┘
                                     │ NativeBridge.kt (+4 funs)
                                     ▼
    JNI bridge (jni_bridge.cpp, +4 exports)   libsfcore.so
                     │
    native core ─────┼── sf_graph.h     (NEW: graph structs, mutators)
                     ├── graph.cpp      (NEW: validation, topo sort, mutators)
                     ├── schema.cpp     (EDIT: signalGraph node/edge rules)
                     └── project.cpp    (EDIT: audit actions +signalGraph.*)
```

- Kotlin treats graph as opaque JSON; **all validation lives in native**. Kotlin assembles typed payloads (node type, params array, edge refs) and reads success/failure from return codes + `lastError`.
- Schema remains the single source of truth. Python `schema.py` reads the same JSON file → drift-free by construction.
- `audioAssets` gains `data.sidecarPath` (optional string); sidecar I/O via `sf_asset_read/write` in `native/src/core/asset_io.cpp` (NEW).

---

## 3. Data Model Changes

### 3.1 Signal Graph (`signalGraph`)
**Current v2 shape** (from `tests/fixtures/project_minimal_v2.json`):
```json
"signalGraph": {
  "nodes": [],
  "edges": []
}
```

**G2 node envelope** (added to `nodes[]`):
```json
{
  "id": "uuid",
  "type": "Input|Gain|EQ|Output",
  "params": [0.0, 0.0, 0.0, 0.0],  // type-specific: gain dB, EQ freq/Q, etc.
  "createdAt": "2026-09-12T00:00:00Z",
  "modifiedAt": "2026-09-12T00:00:00Z"
}
```

**G2 edge envelope** (added to `edges[]`):
```json
{
  "id": "uuid",
  "sourceNodeId": "uuid",
  "targetNodeId": "uuid",
  "sourcePort": 0,
  "targetPort": 0,
  "createdAt": "2026-09-12T00:00:00Z",
  "modifiedAt": "2026-09-12T00:00:00Z"
}
```

### 3.2 C++ Structs (`native/include/soundforge/sf_graph.h`)
```cpp
struct SfGraphNode {
  char id[36];           // UUID
  char type[16];         // "Input"|"Gain"|"EQ"|"Output"
  float params[4];       // type-specific
  char createdAt[32];    // ISO8601
  char modifiedAt[32];
};

struct SfGraphEdge {
  char id[36];
  char sourceNodeId[36];
  char targetNodeId[36];
  int sourcePort;
  int targetPort;
  char createdAt[32];
  char modifiedAt[32];
};

// Validation: returns SF_OK if acyclic, single Output, no dangling edges
sf_result_t sf_graph_validate(const sfcore::SfProjectDoc* doc, char* errBuf, size_t cap);
```

### 3.3 audioAssets Sidecars
**Current v2 shape** (from `sf_internal.hpp` `ObjectEnvelope`):
```json
"audioAssets": [
  {
    "id": "uuid",
    "type": "audioAsset",
    "version": 1,
    "createdAt": "...",
    "modifiedAt": "...",
    "provenance": "created",
    "data": { "localPath": "assets/foo.wav" }  // G0/G1
  }
]
```

**G2 addition** (optional field in `data`):
```json
"data": {
  "localPath": "assets/foo.wav",
  "sidecarPath": "assets/foo.sfasset"  // NEW G2
}
```

No schema version bump — `sidecarPath` is optional; v2 loaders ignore unknown fields (PLAN_G0 §5.2).

---

## 4. Native Core Contracts

### 4.1 Graph Validation (`native/src/graph/graph.cpp`)
```cpp
// Topological sort (Kahn's algorithm) + cycle detection
sf_result_t sf_graph_validate(const sfcore::SfProjectDoc* doc, char* errBuf, size_t cap) {
  // 1. Count Output nodes (must be exactly 1)
  // 2. Build adjacency list from edges
  // 3. DFS cycle detection OR Kahn's algo (indegree-based)
  // 4. Check dangling edges (source/target node exists)
  // 5. Return SF_OK or SF_E_SCHEMA with errBuf message
}
```

### 4.2 Graph Mutators (`native/include/soundforge/sf_project.h` — additive)
```c
// Add node to signalGraph; type is "Input"|"Gain"|"EQ"|"Output"
sf_result_t sf_graph_add_node(sf_project_t* p, const char* type, const float params[4],
                              char* out_node_id, size_t id_cap);

// Remove node (and all connected edges)
sf_result_t sf_graph_remove_node(sf_project_t* p, const char* node_id);

// Connect two nodes (creates edge)
sf_result_t sf_graph_connect(sf_project_t* p, const char* source_id, const char* target_id,
                             int source_port, int target_port, char* out_edge_id, size_t id_cap);

// Disconnect edge by ID
sf_result_t sf_graph_disconnect(sf_project_t* p, const char* edge_id);

// Set node params array
sf_result_t sf_graph_set_node_params(sf_project_t* p, const char* node_id, const float params[4]);
```

**Mutator contract** (mirrors G1 §4.2):
- Reject `p == nullptr` → `SF_E_INVALID_ARG` + `set_last_error`
- Append audit entry `{ts, "user", "signalGraph.node.add"|"signalGraph.edge.connect"|..., objectId:<node/edge id>, detail:"<field>=<value>"}`
- Bump `project.modifiedAt = now_iso8601()`
- Validate graph after mutation (call `sf_graph_validate`); reject if invalid → `SF_E_SCHEMA`

### 4.3 Asset I/O (`native/src/core/asset_io.cpp` — NEW)
```cpp
// Write binary blob to sidecar path
sf_result_t sf_asset_write_blob(const char* sidecar_path, const void* data, size_t len);

// Read binary blob from sidecar path
sf_result_t sf_asset_read_blob(const char* sidecar_path, void* out_buf, size_t cap, size_t* out_len);
```

### 4.4 JNI Bridge (`app/src/main/cpp/jni_bridge.cpp` — EDIT)
**Current exports** (G1 DoD sweep §8): 16
```cpp
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_*:
  // G0 (12): engineVersion, schemaVersion, isCompatible, projectCreate, projectDestroy,
  //          projectToJson, projectFromJson, validateJson, healthCheck, lastError, log, flushLogs
  // G1 (4): renameProject, renameVenue, setVenueDimensions, setSceneGeometry
```

**G2 additions** (4):
```cpp
external fun addGraphNode(handle: Long, type: String, params: FloatArray): String  // returns node UUID
external fun removeGraphNode(handle: Long, nodeId: String): Int
external fun connectGraphEdge(handle: Long, sourceId: String, targetId: String,
                              sourcePort: Int, targetPort: Int): String  // returns edge UUID
external fun disconnectGraphEdge(handle: Long, edgeId: String): Int
```

**Expected export count after G2**: **20** (grep pattern in §8.4)

---

## 5. Kotlin / Compose Changes

### 5.1 `MixerScreen.kt` (EDIT — replace 24-LOC stub)
```kotlin
@Composable
fun MixerScreen(
  projectId: String?,
  projectHandle: Long?,
  signalGraph: SignalGraphKt?,  // NEW typed mirror
  errorMessage: String?,
  onAddNode: (type: String) -> Unit,
  onRemoveNode: (nodeId: String) -> Unit,
  onConnect: (sourceId: String, targetId: String) -> Unit,
  onNavigateBack: () -> Unit,
) {
  Scaffold(
    topBar = { TopAppBar(title = { Text("Mixer — $projectId") }) }
  ) { padding ->
    Column(modifier = Modifier.fillMaxSize().padding(padding)) {
      // Nodes list (LazyColumn)
      signalGraph?.nodes?.forEach { node ->
        ListItem(
          headlineContent = { Text(node.type) },
          supportingContent = { Text("params: ${node.params.contentToString()}") },
          trailingContent = {
            IconButton(onClick = { onRemoveNode(node.id) }) {
              Icon(Icons.Default.Delete, "Remove node")
            }
          }
        )
      }
      // Add node buttons
      Row {
        Button(onClick = { onAddNode("Input") }) { Text("Add Input") }
        Button(onClick = { onAddNode("Gain") }) { Text("Add Gain") }
        Button(onClick = { onAddNode("EQ") }) { Text("Add EQ") }
        Button(onClick = { onAddNode("Output") }) { Text("Add Output") }
      }
      // Validation errors
      if (errorMessage != null) {
        Text(text = errorMessage, color = MaterialTheme.colorScheme.error)
      }
    }
  }
}
```

### 5.2 `SignalEditorScreen.kt` (EDIT — replace 24-LOC stub)
Form-based editor for node params + edge connections (list of edges, source/target dropdowns).

### 5.3 `ProjectViewModel.kt` (EDIT)
Add graph mutator wrappers (mirror G1 `updateSceneGeometry` pattern):
```kotlin
fun addGraphNode(type: String, params: FloatArray) {
  nativeMutex.withLock {
    if (handle == 0L) return@withLock
    val nodeId = NativeBridge.addGraphNode(handle, type, params)
    // nodeId empty on error → read lastError
  }
}
// Similar: removeGraphNode, connectGraphEdge, disconnectGraphEdge
```

### 5.4 Typed Mirrors (NEW)
- `app/ui/signal/SignalGraphKt.kt`: `data class SignalGraphKt(nodes: List<GraphNodeKt>, edges: List<GraphEdgeKt>)`
- `app/ui/signal/GraphNodeKt.kt`: `data class GraphNodeKt(id: String, type: String, params: FloatArray, ...)`
- `app/ui/signal/GraphEdgeKt.kt`: `data class GraphEdgeKt(id: String, sourceNodeId: String, targetNodeId: String, ...)`

---

## 6. Schema & Migration

### 6.1 Schema Version Decision
**Recommendation**: **No v3 migration**  
**Rationale**:
- `signalGraph` already exists in v2 as `{nodes:[], edges:[]}` (PLAN_G0 §3.1, fixture `project_minimal_v2.json`)
- Node/edge envelopes are additions to existing arrays; v2 schema already allows arbitrary objects in `nodes[]`/`edges[]` arrays (currently untyped)
- `sidecarPath` is optional in `audioAssets[].data`; v2 ignores unknown fields (PLAN_G0 §5.2)
- v2 loaders will accept G2 documents; validation rules (acyclic, single Output) are **health checks**, not schema errors (mirrors G1 geometry-out-of-bounds posture)

### 6.2 If Schema Changes Required
If review determines node/edge envelopes need explicit schema validation:
- Bump to **v3** with migration `(2,3)` in `migration.cpp`
- Migration injects defaults: `signalGraph.nodes = []`, `signalGraph.edges = []` (already present, no-op)
- Update `SF_SCHEMA_VERSION` to 3, `tests/fixtures/project_minimal_v3.json`, `tests/golden/schema_golden_v3.json`

---

## 7. Phased Implementation Order

| # | Phase | Files | Verification |
|---|-------|-------|--------------|
| **P1** | Signal graph structs + validation logic | NEW `native/include/soundforge/sf_graph.h`, `native/src/graph/graph.cpp`; EDIT `native/src/core/schema.cpp` (signalGraph validation rules) | • ctest: `test_graph_validation` (cycles, dangling edges, single Output)<br>• `schema.cpp` dump confirms `signalGraph` shape |
| **P2** | Native graph mutators + audit | EDIT `native/include/soundforge/sf_project.h` (+5 mutators), `native/src/core/project.cpp` (audit actions +signalGraph.*); NEW `tests/unit/test_graph_mutators.cpp` | • ctest: `test_graph_mutators` (audit logs, error codes, validation on mutate)<br>• `set_last_error` checks for invalid ops<br>• Audit entry count +5 |
| **P3** | Asset sidecar I/O | NEW `native/src/core/asset_io.cpp`, `native/include/soundforge/sf_asset_io.h`; EDIT `native/src/core/json_codec.cpp` (sidecarPath field) | • ctest: `test_asset_io` (blob roundtrip)<br>• `sf_internal.hpp` confirms `sidecarPath` field |
| **P4** | JNI + Kotlin models | EDIT `app/src/main/cpp/jni_bridge.cpp` (+4 exports), `app/platform/bridge/NativeBridge.kt` (+4 funs); NEW `app/ui/signal/SignalGraphKt.kt`, `GraphNodeKt.kt`, `GraphEdgeKt.kt` | • static review (no SDK)<br>• `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' app/src/main/cpp/jni_bridge.cpp` → expect **20** |
| **P5** | Mixer UI (list editor) | EDIT `app/ui/mixer/MixerScreen.kt` (replace stub), `app/ui/navigation/NavGraph.kt` (wire handle/graph/errorState) | • Android static: `grep -c 'errorMessage' app/ui/mixer/*` ≥1<br>• `ProjectViewModel` uses `nativeMutex` for mutators |
| **P6** | SignalEditor UI (form editor) | EDIT `app/ui/signal/SignalEditorScreen.kt` (replace stub), NEW `app/ui/signal/SignalEditorViewModel.kt` (optional, if separate VM needed) | • static review<br>• NavGraph route unchanged |
| **P7** | Python mirror sync + drift DoD | EDIT `python/soundforge_py/schema.py` (signalGraph node/edge validation), `python/soundforge_py/migrate.py` (v2→v3 if needed) | • pytest: `test_graph_schema.py` (node/edge validation)<br>• `diff -q native/data/schemas/project_schema.json python/soundforge_py/schema.py` = 0 |
| **P8** | Docs + DoD sweep | EDIT `docs/PLAN_G2.md` (this file), NEW `docs/RELEASE_NOTES_G2.md`; gap-check Appendix | • full suites + drift + release notes<br>• tag `g2-complete` |

**Gate status (TBD):** P1–P8 pending.

---

## 8. Verification (Realistic for Environment)

```bash
# 1. Native (Termux/proot: -j2, generous timeouts)
cmake --build native/build -j2 && ctest --test-dir native/build --output-on-failure

# 2. Python
python3 -m pytest tests/python_tests -q

# 3. Schema drift (must print nothing / exit 0)
diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json
# Python mirror must validate the same docs:
python3 -c "import sys,json; sys.path.insert(0,'python'); \
import soundforge_py.schema as s; d=json.load(open(\
'tests/fixtures/project_minimal_v2.json')); assert s.validate_project(d)==[]; \
print('mirror validates v2 fixture')"

# 4. Android (static only — no SDK/NDK in dev container; documented limitation)
grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' \
  app/src/main/cpp/jni_bridge.cpp   # expect 20 (16 G1 + 4 G2)

# 5. Graph validation tests (new ctest)
ctest -R test_graph --output-on-failure  # expect: graph_validation, graph_mutators
```

---

## 9. Definition of Done (Gate G2)

All true on `main`:

1. **Builds:** `cmake --build native/build && ctest --test-dir native/build` and Android static review (no SDK/NDK in dev container; CI runs full build).
2. **Signal graph validation:** `test_graph_validation` passes (cycles rejected, single Output enforced, dangling edges detected).
3. **Graph mutators:** `test_graph_mutators` passes (audit entries appended, `modifiedAt` bumped, validation on mutate rejects invalid graphs).
4. **Asset sidecars:** `test_asset_io` passes (blob roundtrip, `sidecarPath` field persisted).
5. **JNI exports:** `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridgeBridge_'` → **20** (16 G1 + 4 G2).
6. **UI editors:** `MixerScreen.kt` and `SignalEditorScreen.kt` replace stubs (≥50 LOC each, list/form pattern, `errorMessage` display).
7. **No G3+ leakage:** `native/src/dsp/*`, `native/src/audio/*` remain stub-only; no real-time processing lands.
8. **Schema drift:** `diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json` = 0 (or v3 if migration required); Python mirror validates same docs.
9. **Docs:** This file + `docs/RELEASE_NOTES_G2.md` checked in; tag `g2-complete`.

**Exit artifact:** tag `g2-complete`; release notes attach an example `.sfproj` (v2/v3 fixture with signalGraph populated) + ctest count.

---

## Appendix A: Key Decisions

### A.1 Command Queue
**Decision**: Explicitly **out of scope** for G2  
**Rationale**: Audio-thread synchronization requires real-time context (G3 DSP execution). G2 focuses on structural graph definition and UI editing. The existing `nativeMutex` pattern in `ProjectViewModel.kt` (G1.1) suffices for edit-time serialization. A `SfCommandQueue` would force premature threading model decisions without G3's audio pipeline to validate against. Document in `sf_project.h`: `// G2: synchronous calls; G3 introduces SfCommandQueue for audio-thread safety`.

### A.2 audioAssets Sidecars
**Decision**: Implement **minimal sidecar I/O** (read/write `.sfasset` blobs)  
**Rationale**: Required for asset management (PLAN_G0 §3.4); v2 already has `audioAssets[]` with `localPath`. Adding `sidecarPath` as optional field avoids schema version bump (v2 ignores unknown fields). Blob I/O is additive and testable without G3 audio pipeline.

### A.3 Schema Versioning
**Decision**: **No v3 migration** (unless review determines explicit node/edge schema validation required)  
**Rationale**: `signalGraph` uses existing v2 collections; node/edge envelopes are additions to arrays already present. `sidecarPath` is optional. v2 loaders ignore unknown fields (PLAN_G0 §5.2). Validation rules (acyclic, single Output) are **health checks**, not schema errors (mirrors G1 geometry-out-of-bounds posture in PLAN_G1 §9.6).

### A.4 Signal Node Taxonomy
**Decision**: **4 minimal node types** (`Input`, `Gain`, `EQ`, `Output`)  
**Rationale**: Covers core sound-forge use cases (PLAN_G0 §3.1). Extensible via `params[4]` without ABI changes. `Output` node enforces single-sink topology (validation rule).

### A.5 Evaluation Scope
**Decision**: **Structural only** (topology validity, not real-time processing)  
**Rationale**: G2 delivers the graph data model and editing UI. Real-time DSP evaluation requires G3's audio pipeline (Oboe/AAudio, audio thread, DSP kernels). `sf_graph_validate` checks acyclicity, single Output, connected edges — not sample-by-sample processing.

---

## Appendix B: Files to Create vs Edit (Checklist)

**EDIT:**
- `native/include/soundforge/sf_project.h` (+5 graph mutators)
- `native/src/core/project.cpp` (audit actions +signalGraph.*)
- `native/src/core/schema.cpp` (signalGraph node/edge validation rules)
- `native/src/core/json_codec.cpp` (signalGraph node/edge codec, sidecarPath field)
- `app/src/main/cpp/jni_bridge.cpp` (+4 exports)
- `app/platform/bridge/NativeBridge.kt` (+4 funs)
- `app/ui/mixer/MixerScreen.kt` (replace stub)
- `app/ui/signal/SignalEditorScreen.kt` (replace stub)
- `app/ui/navigation/NavGraph.kt` (wire handle/graph/errorState)
- `app/ui/project/ProjectViewModel.kt` (+graph mutator wrappers)
- `python/soundforge_py/schema.py` (signalGraph validation)
- `tests/unit/test_scene_venue.cpp` (add graph mutator tests or separate file)

**NEW:**
- `native/include/soundforge/sf_graph.h`
- `native/src/graph/graph.cpp`
- `native/src/core/asset_io.cpp`
- `native/include/soundforge/sf_asset_io.h`
- `tests/unit/test_graph_validation.cpp`
- `tests/unit/test_graph_mutators.cpp`
- `tests/unit/test_asset_io.cpp`
- `app/ui/signal/SignalGraphKt.kt`
- `app/ui/signal/GraphNodeKt.kt`
- `app/ui/signal/GraphEdgeKt.kt`
- `tests/python_tests/test_graph_schema.py`
- `docs/RELEASE_NOTES_G2.md`

**STUB (unchanged from G0/G1):**
- `native/src/{dsp,audio,acoustics,arrays,power,render,measurement,optimization}/CMakeLists.txt`

> Total G2: ~12 NEW, ~12 EDIT, 0 new STUB.

---

## Appendix C: Open Questions (for Review Gate)

1. **Node/edge schema validation**: Should v2 schema explicitly validate node/edge envelopes, or defer to v3? (Recommendation: v2 health-check only, v3 if explicit schema validation required.)
2. **Graph validation severity**: Should invalid graphs (cycles, multiple Outputs) be Schema Errors or Health Warnings? (Recommendation: Error on mutator write, Warning in health check for legacy docs — mirrors G1 geometry posture.)
3. **Edge port semantics**: Are `sourcePort`/`targetPort` always 0 for G2 (single I/O per node), or should we validate multi-port topologies? (Recommendation: reserve fields, validate as 0 in G2.)
4. **Python mirror drift**: If schema unchanged (v2), does `schema.py` need explicit node/edge validation, or inherit from JSON structural checks? (Recommendation: add explicit validation to match native `sf_graph_validate` rules.)
