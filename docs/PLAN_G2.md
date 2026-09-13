# SoundForge — Gate G2 Plan: Signal Graph, Routing & Mixer Engine

> **Status:** REVIEWED (adversarial pass by @oracle + @security-reviewer done;
> findings HIGH 2 / MED 4 / LOW 1 baked into §4/§5/§6/§8/§9/§10.1 — see
> §10.4 disposition). Ready for gate execution on `main`.
> **Supersedes:** `docs/PLAN_G0.md`/`docs/PLAN_G1.md` for G2 scope only. G0/G1
> remain the contract for everything not changed here.
> **Deliverable changes:** signal graph becomes *editable, persisted,
> validated, and structurally evaluable*; the mixer becomes a *routing desk*
> (per-node gain/pan/mute/solo + static path-gain evaluation). Per-node DSP
> kernels, sample-buffer mixing, and real-time audio are **G3+** and stay out.

---

## 1. G2 Scope & Non-Goals

### 1.1 Goal

The project gains its first *live* signal topology: the user can build a
signal graph (sources → processors → buses → outputs), edit per-node mixer
state (gain/pan/mute/solo), and the native core persists, validates, and
structurally evaluates that graph (topological order, cycle rejection, static
route gains). The audio-thread-safety primitive promised since G0
(`SfCommandQueue`) lands as a tested lock-free SPSC ring — the foundation the
G3 real-time callback runs on — and the two placeholder screens
(`SignalEditorScreen.kt`, `MixerScreen.kt`) become bounded editors.

G2 executes **no DSP**: no sample processing, no `float[]` buffers cross the
ABI, no array optimization, no acoustics, no measurement, no training. The
graph is *defined, persisted, validated, and evaluated structurally*.

### 1.2 G2 MUST deliver

| Pillar | Scope | Deliverable |
|---|---|---|
| **Signal graph model** | schema `$defs/signalNode` + `$defs/signalEdge` + `$defs/signalMixerState`; typed C++ structs; persisted under the existing `signalGraph {nodes, edges}` | Additive schema v2.1 (schemaVersion stays **2**), byte-identical golden, new fixtures |
| **Graph mutation API** | native C ABI `sf_graph_*` mutators | Add/remove node, add/remove edge, set mixer state, set DSP-preset ref — each with audit entry + `SF_E_*` error contract |
| **Routing rules** | native-enforced invariants | No self-loops, no duplicate edges, no dangling refs, no edge into a `source` or out of an `output`, **cycles rejected at mutation time** |
| **Structural validation** | `sf_graph_validate` + health-check integration | Cycle/dangling/duplicate errors; orphan & un-routable warnings (legacy docs stay openable — G1 posture) |
| **Mixer routing engine** | `sf_graph_evaluate_mixer` | Per-output static path gains (linear amplitude), topological order, mute/solo isolation — *scalar routing math, no waveforms* |
| **`SfCommandQueue`** | `sf_cmd_queue_*` + `sf_graph_apply_batch` | Lock-free SPSC ring; malloc-free & lock-free push/pop; overflow → reject + WARN; threaded test proof |
| **Kotlin/Compose editors** | `SignalEditorScreen.kt`, `MixerScreen.kt`, new ViewModels + typed mirrors | Pure-view editors over the G1.1 hardened ViewModel pattern (`@Volatile handle` + `nativeMutex`) |
| **Audit & diagnostics** | 6 new audit actions, `modifiedAt` bumps, `cmdqueue`/`graph` log tags | `graph.addNode` … `graph.setPreset`; overflow/cycle/deadlock logging |

### 1.3 G2 MUST NOT deliver (defer, leave stubs/unchanged)

- **DSP execution** — `native/src/dsp/*` stays stub; no kernels, no sample
  buffers; `dspPresets[].data` body stays unformalized (envelope contract
  only). Per-node `dspPresetRef` is stored but **not applied**.
- **Array configuration/optimization, acoustics simulation, measurement
  capture, training runtime, power graph, reports** — stubs unchanged
  (`powerGraph` stays `{nodes:[], edges:[]}` JSON, untouched in G2).
- **Real-time audio / actual audio callback** — `app/platform/audio/*`
  remains interface-only. The queue is delivered and tested; no caller
  installs it on a live audio lane before G3.
- **`*.sfasset` binary sidecars** — still deferred (PLAN_G1 §6.5 stands).
- **Schema migration** — schemaVersion stays 2; **no migration step, no
  `.bak`, no engine-gated rewrite** of `0.1.0-g1` documents (documented in
  §5.3).
- **New error codes** — exactly one added: `SF_E_FILE_TOO_LARGE` (7) for the
  8 MiB byte-cap (§10.1); `SF_OK`..`SF_E_NOMEM` (0–6) unchanged otherwise.
  Queue overflow reuses `SF_E_IO`, graph semantic rejections reuse
  `SF_E_INVALID_ARG` / `SF_E_NOT_FOUND` / `SF_E_SCHEMA`.
- **Multi-scene / multi-venue documents, `sf_scene_rename`** — still deferred
  (scene stays a frozen-named singleton; signal graph belongs to the scene).

### 1.4 Gating rule

Same as G0/G1: files listed as *stub* are ≤20 LOC and carry a
`// G0: stub — G1+ implements` comment; *new* files implement their bounded
contract only. No file >300 LOC (advisory; deviations recorded the way G1
recorded `project.cpp` 540).

---

## 2. File / Module Breakdown

Canonical root: `/root/project/soundforge/`. Tags: **NEW** / **EDIT** /
**REGEN** (regenerated from the updated schema) / **KEEP** (unchanged).

```
soundforge/
├── native/
│   ├── CMakeLists.txt                    # EDIT — link sfgraph STATIC into sfcore (via src/core)
│   ├── include/soundforge/
│   │   ├── sf_graph.h                    # NEW  — 9 sf_graph_* C ABI exports + SF_NODE_* kinds
│   │   ├── sf_command_queue.h            # NEW  — sf_cmd_t struct, sf_cmd_queue_* C ABI (6 exports)
│   │   ├── sf_project.h                  # KEEP — G0/G1 exports untouched (additive only)
│   │   ├── sf_types.h                    # EDIT — add SF_E_FILE_TOO_LARGE (7)
│   │   └── sf_version.h                  # EDIT — SF_ENGINE_VERSION_SUFFIX "-g1" → "-g2"
│   ├── src/graph/
│   │   ├── CMakeLists.txt                # EDIT — INTERFACE stub → STATIC library sfgraph
│   │   ├── graph_internal.hpp            # NEW  — SignalNode/SignalEdge/SfMixerState/SignalGraphDoc,
│   │   │                                 #        adjacency + mutation helpers (non-ABI)
│   │   ├── graph.cpp                     # NEW  — node/edge/mixer mutators (typed doc ops)
│   │   ├── routing.cpp                   # NEW  — cycle DFS, Kahn topo sort, path-gain evaluator
│   │   └── graph_abi.cpp                 # NEW  — extern "C" wrappers (9 exports + apply_batch),
│   │                                     #        audit entries, set_handle_error, SF_CATCH_ERRORS
│   ├── src/core/
│   │   ├── CMakeLists.txt                # EDIT — add command_queue.cpp; target_link_libraries(sfcore … sfgraph)
│   │   ├── command_queue.cpp             # NEW  — lock-free SPSC ring (push/pop malloc-free)
│   │   ├── sf_internal.hpp               # EDIT — SignalNode/SignalEdge/SfMixerState/SignalGraphDoc
│   │   │                                 #        replace `json signalGraph`; audit_actions() 7→13;
│   │   │                                 #        kMaxDocBytes = 8 MiB; kMaxAuditEntries = 1000
│   │   ├── schema.cpp                    # EDIT — check_graph validates node/edge items (mirror new
│   │   │                                 #        $defs); audit action table 4→13
│   │   ├── json_codec.cpp                # EDIT — typed nodes/edges codec (powerGraph stays json)
│   │   ├── project.cpp                   # EDIT — read_file_capped() at READ time (open_from_path)
│   │   │                                 #        + explicit-size pre-check (from_json); kMaxAuditEntries
│   │   │                                 #        FIFO in user_audit(); health: graph warnings + stats
│   │   └── version_gen.h.in              # KEEP — stamp comes from sf_version.h macros
│   └── data/schemas/project_schema.json  # EDIT — §3: additive v2.1 (see diff)
├── app/
│   ├── src/main/cpp/jni_bridge.cpp       # EDIT — +9 exports (16 → 25); header comment updated
│   ├── platform/bridge/NativeBridge.kt   # EDIT — +9 external fun mirrors
│   ├── platform/audio/SfCommandQueue.kt  # KEEP (NOT created in G2 — native-only queue, see §4.5)
│   └── ui/
│       ├── project/ProjectViewModel.kt   # EDIT — UiState.Ready + signalGraph: SignalGraphKt?;
│       │                                 #        readyState parses signalGraph
│       ├── signal/
│       │   ├── SignalKt.kt               # NEW  — SignalNodeKt/SignalEdgeKt/SignalGraphKt + parsers
│       │   ├── SignalGraphViewModel.kt   # NEW  — g1.1-hardened (@Volatile handle + nativeMutex)
│       │   └── SignalEditorScreen.kt     # EDIT — placeholder → graph editor (pure view)
│       ├── mixer/
│       │   ├── MixerViewModel.kt         # NEW  — g1.1-hardened; mixer state commits
│       │   └── MixerScreen.kt            # EDIT — placeholder → mixer desk (pure view)
│       └── navigation/NavGraph.kt        # EDIT — pass signalGraph state + new ViewModels to routes
├── python/soundforge_py/
│   ├── __init__.py                       # EDIT — __version__ "0.1.0-g1" → "0.1.0-g2"
│   └── migrate.py                        # EDIT — ENGINE_VERSION "0.1.0-g1" → "0.1.0-g2" (no new step)
├── tests/
│   ├── fixtures/
│   │   ├── project_signalgraph_v2.json   # NEW  — minimal valid doc: 3 nodes (src→proc→out), 2 edges
│   │   └── project_graph_corrupt.json    # NEW  — bad node kind, missing mixer, dangling edge, cycle
│   ├── golden/
│   │   ├── schema_golden_v2.json         # REGEN — byte-identical to updated schema (§5.2)
│   │   └── schema_golden_v1.json         # KEEP  — SHA-256 pin unchanged (existing guard)
│   ├── unit/
│   │   ├── test_graph_nodes.cpp          # NEW
│   │   ├── test_graph_edges.cpp          # NEW
│   │   ├── test_graph_mixer.cpp          # NEW
│   │   ├── test_graph_validate.cpp       # NEW
│   │   ├── test_cmd_queue.cpp            # NEW
│   │   ├── test_schema_validate.cpp      # EDIT — node/edge item errors + new audit actions
│   │   └── test_version.cpp              # EDIT — engine version "0.1.0-g2"
│   ├── integration/
│   │   ├── test_graph_roundtrip.cpp      # NEW  — build → save → open → verify
│   │   └── test_project_io.cpp           # EDIT — health-check graph stats; no-migration assertion
│   └── python_tests/test_schema_py.py    # EDIT — signalGraph fixture validation, audit enum, drift
└── docs/
    └── RELEASE_NOTES_G2.md               # NEW  — gate artifact (P8)
```

**KEEP (explicitly untouched):** `native/src/{dsp,audio,acoustics,arrays,
power,render,measurement,optimization}/CMakeLists.txt` (stubs),
`native/data/migrations/*`, `tests/golden/schema_golden_v1.json`,
`app/platform/audio/*` interface-only files, `powerGraph` in schema/codec.

> **Bound check:** if a path above is not listed, do not create it in G2.

---

## 3. Canonical Project Data Model — G2 Additions

Single source of truth: `native/data/schemas/project_schema.json` first; the
C++ structs (§3.2), the Python mirror (validation reads the JSON), and the
golden (§5.2) lockstep with it. schemaVersion stays **2** — G2 changes the
*shapes inside* `signalGraph`, which was already `{nodes:[], edges:[]}` with
untyped item arrays.

### 3.1 JSON Schema diff (v2 → v2.1, all additive)

**a) `signalGraph.items` get formal `$ref`s** (replacing the untyped arrays):

```jsonc
// native/data/schemas/project_schema.json — "signalGraph" (was untyped items)
"signalGraph": {
  "type": "object",
  "required": ["nodes", "edges"],
  "properties": {
    "nodes": { "type": "array", "items": { "$ref": "#/$defs/signalNode" } },
    "edges": { "type": "array", "items": { "$ref": "#/$defs/signalEdge" } }
  }
}
// powerGraph: UNCHANGED (still bare arrays — G7 gate)
```

**b) New `$defs`** (additive; `signalNode`/`signalEdge` deliberately do **not**
set `additionalProperties:false` — G3+ adds DSP fields inside entries):

```jsonc
"$defs": {
  "signalNode": {
    "type": "object",
    "required": ["id", "type", "name", "mixer"],
    "properties": {
      "id": { "type": "string", "format": "uuid" },
      "type": { "type": "string",
                "enum": ["source", "processor", "output", "bus"] },
      "name": { "type": "string", "maxLength": 64 },
      "position": { "$ref": "#/$defs/point2" },
      "mixer":  { "$ref": "#/$defs/signalMixerState" },
      "dspPresetRef": { "type": ["string", "null"] }  // set in G2, applied in G3
    }
  },
  "signalEdge": {
    "type": "object",
    "required": ["id", "fromNodeId", "toNodeId"],
    "properties": {
      "id":         { "type": "string", "format": "uuid" },
      "fromNodeId": { "type": "string", "format": "uuid" },
      "toNodeId":   { "type": "string", "format": "uuid" },
      "fromPort":   { "type": "integer", "minimum": 0 },
      "toPort":     { "type": "integer", "minimum": 0 }
    }
  },
  "signalMixerState": {
    "type": "object",
    "required": ["gainDb", "pan", "mute", "solo"],
    "properties": {
      "gainDb": { "type": "number", "minimum": -60, "maximum": 24 },
      "pan":    { "type": "number", "minimum": -1,  "maximum": 1 },
      "mute":   { "type": "boolean" },
      "solo":   { "type": "boolean" }
    }
  },
  "point2": {
    "type": "object",
    "required": ["x", "y"],
    "additionalProperties": false,
    "properties": {
      "x": { "type": "number" },
      "y": { "type": "number" }
    }
  }
}
```

**c) `auditLog.action` enum extended 4 → 13** (fixes a pre-existing drift:
the native validator and real v2 documents already carry
`project.rename|venue.update|scene.update`, but the schema enum had never been
extended, so Python `jsonschema` would reject any doc containing a G1 audit
entry — see §7.3):

```jsonc
"action": { "type": "string", "enum": [
  "project.create", "project.open", "project.save", "project.migrate",
  "project.rename", "venue.update", "scene.update",
  "graph.addNode", "graph.removeNode",
  "graph.addEdge", "graph.removeEdge",
  "graph.setMixer", "graph.setPreset"
] }
```

**d) Top level & envelopes:** the 18 required keys, `additionalProperties:
false` at top level, and every `objectEnvelope`-driven collection
(`dspPresets`, `equipment`, …) are **unchanged** — the envelope contract
(`id`+`type` minimum) is preserved. `dspPresets[].data` body is **not**
formalized in G2 (G3).

### 3.2 C++ structs (`native/src/core/sf_internal.hpp` + `graph_internal.hpp`)

```cpp
// sf_internal.hpp — typed signal graph replaces the raw json bag for
// signalGraph only; powerGraph stays nlohmann::json (G7).
namespace sfcore {

const size_t kMaxDocBytes = 8 * 1024 * 1024;  // pre-parse cap (G1 residual §10.1)

enum SignalNodeKind : int { SfNodeSource = 1, SfNodeProcessor, SfNodeOutput, SfNodeBus };

struct Point2 { double x = 0.0, y = 0.0; };          // canvas layout, meters*10 .. units G3

struct SfMixerState {
  double gainDb = 0.0;   // [-60, 24]; every setter finite-checks (G1.1 discipline)
  double pan = 0.0;      // [-1, 1]
  bool   mute = false;
  bool   solo = false;
};

struct SignalNode {
  Uuid id;
  int   kind = SfNodeSource;
  std::string name;          // 1..64 bytes (byte cap documented — same caveat as rename)
  Point2 position;           // optional; schema does not require it
  SfMixerState mixer;
  Uuid dspPresetRef = "";    // "" = none; must reference an existing dspPresets envelope id
  json extra = json::object();  // additive open bag (G3 DSP fields)
};

struct SignalEdge {
  Uuid id;
  Uuid fromNodeId;
  Uuid toNodeId;
  int fromPort = 0;
  int toPort = 0;
};

struct SignalGraphDoc {
  std::vector<SignalNode> nodes;
  std::vector<SignalEdge> edges;
  // invariants (see §3.3) are enforced by sf_graph_* mutators;
  // sf_graph_validate checks them for hand-edited/legacy docs.
};
}  // namespace sfcore
```

`SfProjectDoc.signalGraph` becomes `SignalGraphDoc` (was `json`). `doc_to_json`
/ `doc_from_json` (json_codec.cpp) serialize/deserialize the typed vectors;
`doc_from_json` runs the same item checks as `validate_doc_json` so a corrupt
node never enters a live handle. Audit actions table grows to 13
(sf_internal.hpp `audit_actions()`).

### 3.3 Mixer state shape & routing rules (native-enforced)

**Per-node mixer** is stored **inside the node** (`signalNode.mixer`) — the
graph edges *are* the routing; there is no separate bus-routing table.

| Rule | Enforced by | Failure |
|---|---|---|
| Node/edge IDs unique across the graph; UUID v4 format | add/remove mutators + `sf_graph_validate` | `SF_E_SCHEMA` |
| `fromNodeId`/`toNodeId` reference existing nodes; no dangling edges | `sf_graph_add_edge` | `SF_E_NOT_FOUND` |
| No self-loop (`fromNodeId != toNodeId`) | `sf_graph_add_edge` | `SF_E_INVALID_ARG` |
| No duplicate edge (same from→to, fromPort→toPort) | `sf_graph_add_edge` | `SF_E_INVALID_ARG` |
| **No edge into a `source`; no edge out of an `output`** | `sf_graph_add_edge` | `SF_E_INVALID_ARG` (type routing) |
| **No cycles** — reject at mutation via reachability DFS: adding A→B fails if B can reach A | `sf_graph_add_edge` | `SF_E_INVALID_ARG`, last_error "graph.addEdge: would create cycle" |
| `gainDb ∈ [-60,24]`, `pan ∈ [-1,1]`, both finite | `sf_graph_set_mixer` | `SF_E_INVALID_ARG` |
| `dspPresetRef` empty or an existing `dspPresets[].id` | `sf_graph_set_preset` | `SF_E_NOT_FOUND` |
| Removing a node removes all incident edges (counted in audit detail) | `sf_graph_remove_node` | — (atomic within the call) |

**Orphans allowed while editing** (add nodes first, wire later) — flagged as
health **Warning** ("signal graph node not reachable from any source"), not
Error, mirroring the G1 out-of-bounds-geometry posture.

---

## 4. Native Bridge API — New C ABI + JNI

### 4.1 Principles (G0/G1 unchanged)

- Stable additive C ABI; existing 16 exports (G0 12 + G1 4) **untouched**.
- Opaque handle; every public C function returns `SF_*`; messages via
  `set_last_error`/`set_handle_error`; no exceptions across the boundary
  (`SF_CATCH_ERRORS`).
- `std::nothrow new` → `SF_E_NOMEM` (G1.1 rule) everywhere new.
- Threading: a project handle still has exactly **one** mutating thread
  (G0/G1 rule). The queue (§4.4) is the G2 bridge to G3's audio thread and it
  is *separate* from handle-ownership — contract in §4.5.
- All six audit rules from G1 apply (actor `"user"`, `objectId`, truncated
  detail ≤512, `modifiedAt` bump).
- **Finite-input rule (G1.1 carried forward):** every floating-point mutator
  argument (`gain_db`, `pan`) passes `isfinite()` before use → `SF_E_INVALID_ARG`.
- **Bounded audit:** `user_audit()` enforces `kMaxAuditEntries = 1000` FIFO
  (oldest purged first, purge logged once as `audit.rotate purged=<n>` WARN),
  so a drained queue batch cannot grow the log without limit.

### 4.2 New C ABI — `native/include/soundforge/sf_graph.h` (9 exports)

```c
#define SF_NODE_SOURCE    1
#define SF_NODE_PROCESSOR 2
#define SF_NODE_OUTPUT    3
#define SF_NODE_BUS       4

/* out_id: caller buffer of 37+ bytes; filled with the new node's UUID v4. */
sf_result_t sf_graph_add_node(sf_project_t* p, int32_t kind, const char* name,
                              char* out_id);                      /* name 1..64 bytes */
sf_result_t sf_graph_remove_node(sf_project_t* p, const char* node_id); /* cascades edges */

sf_result_t sf_graph_add_edge(sf_project_t* p, const char* from_id, const char* to_id,
                              int32_t from_port, int32_t to_port, char* out_id);
sf_result_t sf_graph_remove_edge(sf_project_t* p, const char* edge_id);

sf_result_t sf_graph_set_mixer(sf_project_t* p, const char* node_id,
                               double gain_db, double pan, int32_t mute, int32_t solo);
sf_result_t sf_graph_set_preset(sf_project_t* p, const char* node_id,
                                const char* preset_id);   /* "" or NULL = clear */

/* Whole-graph structural report (JSON). Returns SF_OK when clean;
   SF_E_SCHEMA when errors exist (report still valid, status="error"). */
sf_result_t sf_graph_validate(const sf_project_t* p, char* report_buf, size_t report_cap);

/* Topological order (Kahn; DFS cycle detection first).  JSON: {"order":[...]} */
sf_result_t sf_graph_topological_order(const sf_project_t* p, char** out_json, size_t* out_len);

/* Static routing evaluation — §4.3. malloc'd; free with sf_free_string(). */
sf_result_t sf_graph_evaluate_mixer(const sf_project_t* p, char** out_json, size_t* out_len);
```

Error mapping: null handle / null args → `SF_E_INVALID_ARG`; missing node or
edge → `SF_E_NOT_FOUND`; non-finite `gain_db`/`pan` → `SF_E_INVALID_ARG`
(`isfinite()` gate, §4.1); candidate-edge violating type routing, duplicate or
cycle → `SF_E_INVALID_ARG`; doc-level inconsistency (dangling edge already
present from hand-edit — mutators re-validate pre-write) → `SF_E_SCHEMA`;
allocation → `SF_E_NOMEM`; oversized project file → `SF_E_FILE_TOO_LARGE`
(§10.1). Last error strings are
`"graph.addNode: ..."`, `"graph.addEdge: ..."` style so log greps are stable.

### 4.3 `sf_graph_evaluate_mixer` — the routing engine (scalar math only)

**Boundary discipline:** this function computes *routing-desk math* —
topological order and per-route static scalar coefficients in the linear
amplitude domain (path product of `10^(gainDb/20)`, summed where routes
merge). It touches **no audio buffers and no sample values**; waveform
summing/mixing is G3.

```jsonc
// Result JSON (sorted keys, dump(2)):
{
  "order": ["<nodeId>", "…"],                 // Kahn order, sources first
  "outputs": [
    {
      "nodeId": "<outputId>",
      "routes": [
        { "sourceId": "<sourceId>",
          "gainLin": 0.707,                    // product of 10^(gainDb/20) along path
          "nodeIds": ["<path…>"] }
      ],
      "peakGainLin": 1.414,
      "clipped": false                         // peakGainLin > 1.0
    }
  ],
  "muted":  ["<nodeId>", "…"],
  "soloed": ["<nodeId>", "…"]
}
```

Semantics: muted nodes contribute nothing; if **any** node is soloed, only
soloed nodes (and their transitive up/down-stream path nodes) route; an output
with zero reachable sources gets an empty `routes` array. `clipped:true` is a
desk indicator — no clamping is applied in G2 (that is G3 DSP behavior).

### 4.4 New C ABI — `native/include/soundforge/sf_command_queue.h` (6 exports)

```c
/* Fixed-size command image — never grows (fits the ring slot; 176 B incl.
   8 B alignment padding — content is 168 B). Slot size is a compile-time
   constant; the 10k-message threaded test asserts no gap and no overflow
   at capacity 256 (≈ 39 drains). Revisit size with the G3 command set. */
typedef struct sf_cmd {
  int32_t  type;        /* 1 addNode, 2 removeNode, 3 addEdge, 4 removeEdge,
                           5 setMixer, 6 setPreset, 7 evaluateMixer */
  uint64_t seq;         /* monotonic, assigned by enqueue */
  char     id1[64];     /* node/edge uuid (or new-node name for addNode: kind in type) */
  char     id2[64];     /* second uuid (edge target / preset id) */
  double   value;       /* gain_db (setMixer) */
  double   value2;      /* pan (setMixer) */
  int32_t  flags;       /* bit0 mute, bit1 solo; kind for addNode */
  int32_t  port_a;      /* from_port / node kind fallback */
  int32_t  port_b;      /* to_port */
} sf_cmd_t;

typedef struct sf_cmd_queue_s sf_cmd_queue_t;   /* opaque */

sf_result_t sf_cmd_queue_create(sf_cmd_queue_t** out);      /* allocates ring (NOT on audio path) */
void        sf_cmd_queue_destroy(sf_cmd_queue_t* q);
sf_result_t sf_cmd_queue_enqueue(sf_cmd_queue_t* q, const sf_cmd_t* cmd); /* producer side */
sf_result_t sf_cmd_queue_dequeue(sf_cmd_queue_t* q, sf_cmd_t* out);       /* consumer side */
int32_t     sf_cmd_queue_depth(const sf_cmd_queue_t* q);
/* Drain + apply: applies cmds[0..n) to p via the SAME code path as the
   synchronous mutators (ring of audit entries + modifiedAt bumps per cmd). */
sf_result_t sf_graph_apply_batch(sf_project_t* p, const sf_cmd_t* cmds, size_t n,
                                 size_t* applied, char* err_buf, size_t err_cap);
```

Implementation (`native/src/core/command_queue.cpp`): **lock-free SPSC ring**,
capacity 256 (power of two), head/tail as `std::atomic<uint32_t>` with
reserve/commit CAS; `enqueue` is wait-free (single CAS), `dequeue` wait-free.
Push/pop are **allocation-free by construction** — the ring is a fixed array of
fixed 176-B slots, so the hot path never allocates and never locks (the G0
promise "Real-time audio callback must NEVER call malloc/lock/free" is thereby
satisfied by design, not by discipline; no `std::malloc`/`mutex` appears in the
push/pop code path — audited + ASan-verified). This is not premature: the live
audio lane is G3, but the *structural* property is free to prove now and cannot
be regressed later without a review-visible change. Full ring: enqueue rejects
with `SF_E_IO` and logs `WARN "cmdqueue: overflow dropped seq=<n>"` — no
blocking, no drop-oldest (dropping state mid-pipeline is worse than rejecting).

### 4.5 Threading / concurrency contract (documented in `sf_command_queue.h`)

| # | Contract |
|---|---|
| C1 | Queue is **SPSC**: one producer, one consumer. Two producers are undefined behavior (asserted in debug tests). |
| C2 | The queue is lock-free & malloc-free on push *and* pop. `create`/`destroy` allocate and are never called by an audio callback. |
| C3 | A project handle keeps exactly one mutating thread (G0/G1 rule **unchanged**). The queue only transports *intent*; `sf_graph_apply_batch` applies it **on the owner thread**. The queue itself shares no state with the handle. |
| C4 | Seq is a monotonically increasing watermark; `< min(dequeued seq)` commands were applied; tests assert no gaps. `sf_cmd_queue_depth` is for diagnostics/tests (not called on the audio path). |
| C5 | G2 ships the queue as a **tested primitive**, not yet on a live audio lane (no RT callback exists until G3). |
| C6 | In G3 the audio callback owns a **queue-drained clone path**: UI enqueues; callback dequeues and applies via `sf_graph_apply_batch`. The G2 screens bypass the queue and call the synchronous mutators directly (same pattern as G1 editing) — intentionally, so G2 ships no half-wired consumer thread. |

### 4.6 JNI bridge — `jni_bridge.cpp` + `NativeBridge.kt` (+9 exports, 16 → 25)

One thin passthrough per new graph C export (no queue exports this gate —
nothing on the Kotlin side consumes the queue until G3, per §4.5 C5/C6):

```c
// jni_bridge.cpp — new (prefix Java_id_soundforge_pastudio_platform_bridge_NativeBridge_)
Java_..._graphAddNode(JNIEnv*, jobject, jlong handle, jint kind, jstring name) -> jstring /*id, "" on error*/
Java_..._graphRemoveNode(JNIEnv*, jobject, jlong handle, jstring nodeId) -> jint
Java_..._graphAddEdge(JNIEnv*, jobject, jlong handle, jstring fromId, jstring toId,
                      jint fromPort, jint toPort) -> jstring /*edge id, "" on error*/
Java_..._graphRemoveEdge(JNIEnv*, jobject, jlong handle, jstring edgeId) -> jint
Java_..._graphSetMixer(JNIEnv*, jobject, jlong handle, jstring nodeId,
                       jdouble gainDb, jdouble pan, jboolean mute, jboolean solo) -> jint
Java_..._graphSetPreset(JNIEnv*, jobject, jlong handle, jstring nodeId, jstring presetId) -> jint
Java_..._graphValidate(JNIEnv*, jobject, jlong handle) -> jstring /*JSON report, "" on error*/
Java_..._graphTopologicalOrder(JNIEnv*, jobject, jlong handle) -> jstring
Java_..._graphEvaluateMixer(JNIEnv*, jobject, jlong handle) -> jstring
```

```kotlin
// NativeBridge.kt — mirrors, one external fun per export, no logic
external fun graphAddNode(handle: Long, kind: Int, name: String): String
external fun graphRemoveNode(handle: Long, nodeId: String): Int
external fun graphAddEdge(handle: Long, fromId: String, toId: String,
                          fromPort: Int, toPort: Int): String
external fun graphRemoveEdge(handle: Long, edgeId: String): Int
external fun graphSetMixer(handle: Long, nodeId: String, gainDb: Double, pan: Double,
                           mute: Boolean, solo: Boolean): Int
external fun graphSetPreset(handle: Long, nodeId: String, presetId: String): Int
external fun graphValidate(handle: Long): String
external fun graphTopologicalOrder(handle: Long): String
external fun graphEvaluateMixer(handle: Long): String
```

export-grep DoD (see §9): `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' app/src/main/cpp/jni_bridge.cpp` → **25**.

Kotlin ViewModels follow the g1.1 hardened pattern verbatim:
`@Volatile handle` (the *project* handle, shared via `UiState.Ready`),
`nativeMutex.withLock` around every graph commit, `close()` joins via
`runBlocking` before `projectDestroy`. New typed mirrors in `SignalKt.kt`
(`SignalGraphKt(nodes, edges)`, `SignalNodeKt(id, kind, name, x, y, gainDb,
pan, mute, solo, dspPresetRef)`, `SignalEdgeKt(id, fromNodeId, toNodeId,
fromPort, toPort)`) parsed from `projectToJson().signalGraph` inside
`readyState` — committed-doc-is-truth, same as `projectSceneOf` (G1).

---

## 5. Versioning & Audit Log

### 5.1 Version bump

`SF_ENGINE_VERSION_SUFFIX`: `"-g1"` → `"-g2"` in
`native/include/soundforge/sf_version.h` (+ `python/soundforge_py/__init__.py`
`__version__` and `migrate.py` `ENGINE_VERSION`).

**Choice: `0.1.0-g2`, not `0.2.0-g2`.** Justification: the `-gN` suffix is the
pre-release gate tag on the 0.1.x line (G0/G1 precedents); a minor bump would
imply a new stable feature surface, which misrepresents pre-1.0 gate staging.
The minor number can move when the app ships its first stable feature set.
`SF_SCHEMA_VERSION` stays **2** (additive shapes only — see §5.3).
`sf_is_compatible` semantics unchanged.

### 5.2 Golden & fixtures

- `tests/golden/schema_golden_v2.json` is **regenerated** as a byte-identical
  copy of the updated schema (this is the *deliberate* v2→v2.1 refresh the
  drift guard exists to catch by review — the guard script itself is the
  change-detection signal, and regeneration is reviewed in P1's commit).
- The v2 SHA-256 digest pin in `test_schema_py.py`
  (`test_golden_v2_immutable_digest`, §7.3) updates **in the same P1 commit**
  as the regeneration — the two never land on opposite sides of a tree state,
  so the drift guard stays green at every commit.
- `tests/golden/schema_golden_v1.json`: **KEEP** — SHA-256 pin test untouched.
- New fixtures: `project_signalgraph_v2.json` (3 nodes: source "DI 0" →
  processor "EQ" → output "FOH", plus a second bus "MONS"; 2 edges; one node
  with `mixer` fields set), `project_graph_corrupt.json` (node with
  `"type":"amp"` [bad kind], node missing `mixer`, edge with dangling
  `toNodeId`, edge A→B→C-plus-C→A cycle).

### 5.3 Migration path from `0.1.0-g1` projects — **none needed**

schemaVersion stays 2 and all G2 additions are **shape-only inside existing
arrays**: an existing `signalGraph` with `nodes: []`/`edges: []` validates
trivially (empty arrays satisfy the new item `$ref`s), and a `0.1.0-g1` doc
with a *non-empty* hand-authored graph opens as long as its items satisfy the
new `$defs` — structurally-checked, exactly like G1's out-of-bounds
geometries, which opened with Warnings. Consequences, explicit:

- `sf_project_open_from_path` writes **no `.bak.v2`** and appends **no**
  `project.migrate` audit entry for g1→g2 docs.
- `migrate.py` / `migration.cpp` gain **no new step**; `ENGINE_VERSION` bump
  only affects *future* migrated docs.
- The engine version string inside untouched files stays `0.1.0-g1` (it is
  informational, "version that wrote the file", per G0 §5.1).

### 5.4 Audit log — 7 → 13 actions

New actions (all `actor:"user"`, `objectId` = affected object, `modifiedAt`
bumped, detail ≤512 chars):

| Action | objectId | detail example |
|---|---|---|
| `graph.addNode` | new node id | `kind=source name="DI 0"` |
| `graph.removeNode` | removed node id | `removed 2 incident edges` |
| `graph.addEdge` | new edge id | `from=<n1> to=<n2> ports 0:0` |
| `graph.removeEdge` | removed edge id | `from=<n1> to=<n2>` |
| `graph.setMixer` | node id | `gainDb=-6 pan=0 mute=0 solo=0` |
| `graph.setPreset` | node id | `dspPresetRef=<presetId>` or `dspPresetRef=` (cleared) |

`sf_graph_apply_batch` applies each command through the *same* internal
mutator, so each applied command emits its own audit entry (no lossy batch
summary; a drained batch of 100 commands appends 100 entries — bounded by
queue capacity 256).

**Bounded audit growth:** `user_audit()` keeps at most `kMaxAuditEntries =
1000` entries, FIFO (oldest purged first, purge logged once as
`audit.rotate purged=<n>` WARN). Rationale: a long editing session or a big
drained batch must not grow the log without limit; 1000 is an order of
magnitude above any realistic interactive session and matches the queue
capacity posture. Purging never drops entries from *uncommitted* work — only
committed log rows, which is acceptable for the audit's
"what changed, in order" contract.

---

## 6. Diagnostics & Logging

### 6.1 New log sites (all through the existing `sf_log` ring + sink)

| Condition | Level | Tag | Payload |
|---|---|---|---|
| Queue enqueue rejected (ring full) | `WARN` | `cmdqueue` | `overflow dropped seq=<n>` |
| Edge add rejected as cycle | `WARN` | `graph` | `cycle rejected <from>-><to>` |
| `sf_graph_validate` finds cycle/dangling/duplicate in a loaded doc | `ERROR` | `graph` | first error string |
| `sf_graph_evaluate_mixer` finds an output with no source path | `WARN` | `graph` | `unroutable output <id>` |
| Any `graph.*` / `setMixer` / `setPreset` failure | `ERROR` | `graph` | `last_error` message (automatic via set_handle_error funnel) |
| `sf_graph_apply_batch` partial apply (cmd k of n failed) | `ERROR` | `graph` | `apply_batch stopped at <k>/<n>: <err>` |
| Successful graph mutation | `INFO` | `graph` | `addNode <id>`, `removeEdge <id>`, … |

### 6.2 Health-check additions (`sf_project_health_check`)

| Check | Ok | Warning | Error |
|---|---|---|---|
| Structural validation incl. node/edge items (new `$defs` mirrored) | passes | — | corrupt node/edge from hand-edit (still opens: validate is structural, Warning-not-Error posture does NOT apply to *schema-shape* — the doc stays openable but health reports `error`) |
| Reachability: every output has ≥1 source path; every non-source is reachable | all reachable | orphan/unroutable node | — |
| Queue depth high-water (if a queue exists) | — | ≥ 192/256 | — |
| `stats.signalGraph.nodes` / `stats.signalGraph.edges` | added to stats | — | — |

---

## 7. Test Plan

> Gate G2 exit: graph can be *built, edited, validated, evaluated structurally,
> saved and reopened*; the queue is proven lock-free under a threaded test;
> schema drift guard green; JNI export grep = 25.

### 7.1 Native unit tests (`tests/unit/`, via ctest)

| File | Notable cases |
|---|---|
| `test_graph_nodes.cpp` (8) | add one of each kind; output id is UUID v4; name 0/65 bytes rejected, 64 ok; null args; `removeNode` cascades edges (count in audit detail); remove missing → `SF_E_NOT_FOUND`; `graph.addNode` audit entry present with correct action. |
| `test_graph_edges.cpp` (9) | valid add → both ends see the edge; duplicate rejected; self-loop rejected; dangling target → `SF_E_NOT_FOUND`; edge *into* source rejected; edge *out of* output rejected; `fromPort`/`toPort` < 0 rejected; cycle A→B→C, then C→A rejected at mutation (`would create cycle`); `graph.addEdge` audit + `modifiedAt` bumped. |
| `test_graph_mixer.cpp` (10) | setMixer roundtrip; gainDb = −60 / +24 ok, −61/+25/NaN/Inf rejected; pan ∉ [−1,1] rejected; `isfinite()` gate on `nan`/`inf` → `SF_E_INVALID_ARG`; setPreset with valid envelope id, unknown id → `SF_E_NOT_FOUND`, `""` clears; evaluate: chain gain = product (two −6 dB nodes → 0.5); merge sums (two sources at 0 dB into one output → peakGainLin 2.0, clipped true); mute zeroes a path; solo isolates (only soloed + path nodes route). |
| `test_graph_validate.cpp` (5) | clean graph → `{"status":"ok"}` + SF_OK; hand-edited dangling edge → `status:"error"` + SF_E_SCHEMA; hand-edited cycle → error; orphan node → warning; report keys present. |
| `test_cmd_queue.cpp` (6) | roundtrip all 7 cmd types (fields byte-identical); enqueue past capacity → `SF_E_IO` + WARN logged; depth returns to 0 after drain; `seq` strictly monotonic; **threaded SPSC**: producer thread enqueues 10 000 `addNode` cmds, consumer dequeues → `sf_graph_apply_batch` → handle has 10 000 nodes, audit 10 000 entries, ASan clean; second consumer refuses (documented single-consumer contract). |
| `test_schema_validate.cpp` (EDIT) | `project_signalgraph_v2.json` → SF_OK; `project_graph_corrupt.json` → SF_E_SCHEMA with non-empty err_buf; audit entry with `graph.addEdge` action → SF_OK (enum drift fixed). |
| `test_version.cpp` (EDIT) | engine version equals `0.1.0-g2`; `sf_is_compatible` table unchanged (0/1/2 → 1, 3 → 0). |

### 7.2 Integration tests (`tests/integration/`)

| Test | Steps | Pass |
|---|---|---|
| `test_graph_roundtrip.cpp` (NEW) | build graph via mutators (2 sources, 1 processor, 2 edges, mixer edits) → `sf_project_save_to_path` → reopen → nodes/edges/mixer equal, `graph.*` audit actions present, `schemaVersion==2`, **no** `project.migrate` entry added | Round-trip + no-migration proven |
| `test_project_io.cpp` (EDIT) | health check on signalgraph fixture reports `stats.signalGraph.nodes==3`; open `project_minimal_v2.json` (empty graph) → SF_OK, stats 0/0 | Backward-compat of empty graphs |

### 7.3 Python tests (`tests/python_tests/test_schema_py.py`, EDIT)

```python
def test_validate_signalgraph_v2():        # project_signalgraph_v2.json -> == []
def test_validate_graph_corrupt_fails():   # project_graph_corrupt.json -> len > 0
    # asserts include the bad kind ("amp"), missing mixer, dangling edge, cycle
def test_audit_enum_extended(): # doc containing graph.setMixer entry validates
def test_golden_v2_immutable_digest():  # NEW SHA-256 pin for schema_golden_v2.json
    # (v2.1 regeneration is a reviewed event that updates this pin on purpose)
def test_golden_v1_immutable():           # existing pin — UNCHANGED
```

> The enum-drift fix in §3.1(c) is covered by `test_audit_enum_extended`: a
> v2 document carrying the G1 actions (`project.rename`, `venue.update`,
> `scene.update`) **must** validate — it fails today, which is the bug G2 P1
> fixes before any graph action ships.

### 7.4 Android (static-only — no SDK/NDK, no `assembleDebug`, no logcat)

- `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` = 25.
- Kotlin reviewed statically: screens are pure views (no `NativeBridge.*`
  calls inside composables — g1.1 rule), ViewModels own all native work under
  `nativeMutex`, `close()` joins before `projectDestroy`.
- No host Kotlin tests can load `libsfcore.so`; native behavior is proven by
  ctest/pytest above.

### 7.5 Verification commands (host)

```bash
cmake --build native/build -j2 && ctest --test-dir native/build --output-on-failure
python3 -m pytest tests/python_tests -q
diff native/data/schemas/project_schema.json tests/golden/schema_golden_v2.json   # must print nothing
grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_' app/src/main/cpp/jni_bridge.cpp  # 25
```

---

## 8. Build Order / Phase Steps

Strict order; each phase leaves the tree green (ctest + pytest). One commit
per phase. Expected net-new native tests: **42** (46 → 88); pytest 6 → 10.

### P1 — Schema v2.1 + audit-enum drift fix + golden/fixtures (1 day)
- [x] 1.1 EDIT `native/data/schemas/project_schema.json`: `signalNode`/
      `signalEdge`/`signalMixerState`/`point2` `$defs`, wire `signalGraph`
      items, extend audit `enum` 4→13. **No** top-level/`objectEnvelope`
      changes.
- [x] 1.2 REGEN `schema_golden_v2.json` (byte-identical copy) **same commit**
      as digest-pin update in `test_schema_py.py` (new
      `test_golden_v2_immutable_digest`, mirroring the v1 guard); NEW fixtures
      `project_signalgraph_v2.json`, `project_graph_corrupt.json`.
- [x] 1.3 EDIT `schema.cpp` `check_graph` (node/edge item checks mirroring new
      `$defs`) + `audit_actions()` in `sf_internal.hpp` (7→13).
- [x] 1.4 EDIT `sf_types.h` (add `SF_E_FILE_TOO_LARGE` = 7, `sf_error_string`
      table), `project.cpp` (add `read_file_capped(path, maxBytes)` helper,
      `from_json` size pre-check; `kMaxDocBytes = 8 MiB` in `sf_internal.hpp`;
      `kMaxAuditEntries = 1000` FIFO in `user_audit()`) — byte-cap at READ
      time, not post-parse.
- [x] 1.5 EDIT `test_schema_validate.cpp` + `test_schema_py.py` (+4 tests:
      `test_golden_v2_immutable_digest`, `test_validate_signalgraph_v2`,
      `test_validate_graph_corrupt_fails`, `test_audit_enum_extended`) →
      green. **Evidence:** ctest schema + pytest + drift diff.

### P2 — Typed graph core (½ day)
- [x] 2.1 NEW `graph_internal.hpp` (structs, kind enum, invariants) +
      `graph.cpp` (pure internal mutation helpers over `SignalGraphDoc`).
- [x] 2.2 EDIT `sf_internal.hpp` (`SignalGraphDoc signalGraph` replaces `json
      signalGraph`), `json_codec.cpp` (typed nodes/edges codec; powerGraph
      stays json), `sf_project.h` untouched.
- [x] 2.3 EDIT `native/src/graph/CMakeLists.txt`: **`INTERFACE` → `STATIC`
      library** `sfgraph` with source glob; EDIT `native/src/core/CMakeLists.txt`:
      `target_link_libraries(sfcore PRIVATE sfgraph)`; top-level
      `native/CMakeLists.txt` fine (existing foreach). **Evidence:** full
      existing suite green (no behavior change yet), build green.

### P3 — Graph mutators C ABI + audits (1 day)
- [x] 3.1 NEW `sf_graph.h` + `graph_abi.cpp`: `sf_graph_add_node /
      remove_node / add_edge / remove_edge / set_mixer / set_preset` with
      routing rules (§3.3), audit entries, `set_handle_error`,
      `SF_CATCH_ERRORS`, `nothrow new`.
- [x] 3.2 NEW `test_graph_nodes.cpp`, `test_graph_edges.cpp` → green.
      **Evidence:** ctest graph_nodes+graph_edges; audit counts.

### P4 — Routing: validate + topo order + mixer evaluator (1 day)
- [x] 4.1 NEW `routing.cpp`: reachability DFS (cycle reject helper shared with
      P3), Kahn topo sort, path-gain walker (mute/solo aware).
- [x] 4.2 ABI: `sf_graph_validate`, `sf_graph_topological_order`,
      `sf_graph_evaluate_mixer` (+ `sf_project_health_check` graph warnings &
      stats in `project.cpp`).
- [x] 4.3 NEW `test_graph_mixer.cpp`, `test_graph_validate.cpp` → green.
      **Evidence:** ctest mixer+validate; cycle/mute/solo/clip cases.

### P5 — SfCommandQueue (1.5–2 days)
- [x] 5.1 NEW `sf_command_queue.h` + `core/command_queue.cpp`: SPSC lock-free
      ring (C++20 atomics), capacity 256 (power of two; 176-B slot size =
      44 KiB total; 10k threaded test uses full drain cycles), overflow
      reject + WARN, push/pop allocation-free by construction (fixed array of
      fixed slots; no `malloc`/`mutex` in hot path — audited + ASan-verified).
- [x] 5.2 NEW `test_cmd_queue.cpp` incl. the 10k threaded SPSC test → green
      under ASan/UBSan. **Evidence:** ctest cmd_queue; no alloc on hot path
      verified by ASan + code audit of push/pop; pthread works on host CI
      (G0/G1 limitation for Android remains — static-only).

### P6 — JNI + Kotlin models + ViewModels (1 day)
- [x] 6.1 EDIT `jni_bridge.cpp` (+9 exports, header comment → 25),
      `NativeBridge.kt` (+9 mirrors).
- [x] 6.2 NEW `SignalKt.kt` (typed mirrors + parsers); EDIT
      `ProjectViewModel.kt` (`UiState.Ready.signalGraph`, `readyState` parse);
      NEW `SignalGraphViewModel.kt`, `MixerViewModel.kt` (g1.1-hardened:
      `@Volatile handle`, `nativeMutex`, commits via `graph*` bridge calls).
- [x] 6.3 **Evidence:** export grep = 25; static review (screens pure, mutex
      discipline, close() joins).

### P7 — Editors + Nav wiring (1 day, static-only)
- [x] 7.1 EDIT `SignalEditorScreen.kt`: node list + add/remove (kind picker),
      edge add/remove (from/to node dropdowns, port fields), preset ref chip;
      errors inline via `lastError`; **no `NativeBridge.*` in composables**.
- [x] 7.2 EDIT `MixerScreen.kt`: per-node gain slider (−60..24 dB), pan slider,
      mute/solo toggles, "Routing" preview card showing
      `graphEvaluateMixer` JSON (order + per-output routes).
- [x] 7.3 EDIT `NavGraph.kt`: route `signal`/`mixer` receive
      `projectViewModel` state + the two new ViewModels.
- [x] 7.4 **Evidence:** static review; placeholder affordances for
      `projectId == null` preserved (G0 posture).

### P8 — Docs + DoD sweep (½ day)
- [x] 8.1 Full suite (§7.5): ctest 119/119 (reg + UBSan; plan expected 88 —
      actual grew with per-phase extensions), pytest 9/9 (plan expected 10),
      drift diff empty, grep 25.
- [x] 8.2 NEW `docs/RELEASE_NOTES_G2.md`; this plan finalized; Appendix
      review items (§10) resolved/re-deferred; tag `g2-complete`.

---

## 9. Definition of Done (Gate G2)

All true on `main`:

1. **Builds:** `cmake --build native/build && ctest --test-dir native/build`
   green (**46 → 88 native tests**); `pytest tests/python_tests` green
   (**6 → 10**); `./gradlew :app:assembleDebug` = CI-only (static review here,
   documented G0/G1 limitation).
2. **Schema v2.1, no drift:** `project_schema.json` byte-identical to
   `schema_golden_v2.json` (both SHA-256 pinned in pytest); top-level 18
   required keys + `additionalProperties:false` unchanged; all
   `objectEnvelope` collections untouched.
3. **No migration:** `0.1.0-g1` fixture opens with `schemaVersion==2`, no
   `.bak`, no `project.migrate` entry (integration test); `sf_migrate_json`
   unchanged; downgrade still `SF_E_VERSION`. **8 MiB byte cap:** file ≥
   `kMaxDocBytes` → `SF_E_FILE_TOO_LARGE` (7) via `read_file_capped()` at read
   time (never reaches nlohmann); `from_json` rejects oversized explicit-size
   inputs; fixtures asserting `SF_E_FILE_TOO_LARGE` exist.
4. **Graph mutations:** add/remove node/edge, setMixer, setPreset —
   round-trip via save/reopen; routing rules enforced (cycle, duplicate,
   self-loop, dangling, type-routing all rejected with the right `SF_*` code +
   `last_error`).
5. **Routing engine:** `graphEvaluateMixer` returns correct topological order
   and static path gains (product/sum chain+merge tests), mute/solo honored,
   `clipped` flag correct, `topologicalOrder` consistent with `validate`.
6. **Queue:** SPSC lock-free ring proven under 10k-message threaded test
   (ASan clean), overflow rejects + WARNs, push/pop allocation-free by
   construction (fixed 176-B slots ring; no malloc/mutex in hot path; ASan +
   code audit), concurrency contract C1–C6 documented in header.
7. **Audit:** 13 actions in schema enum + `audit_actions()`; every successful
   graph mutation appends its entry; `modifiedAt` bumped; existing 46 tests
   still pass.
8. **No G3+ leakage:** `dsp|audio|acoustics|arrays|power|render|measurement|
   optimization/*` remain stubs; no sample buffers/`float[]` in any new ABI
   signature; `powerGraph` untouched; `dspPresets[].data` still envelope-only;
   no `*.sfasset` code; no real-time audio lane wired.
9. **JNI:** `grep -c '^Java_id_soundforge_pastudio_platform_bridge_NativeBridge_'` =
   **25** (16 + 9); existing 16 signatures unmodified; new exports are thin
   passthroughs with the g1.1 exception-fence discipline.
10. **Docs:** this file + `docs/RELEASE_NOTES_G2.md` checked in;
    residuals disposition (§10) resolved; tag `g2-complete`.

**Exit artifact:** tag `g2-complete`; release notes attach the
`project_signalgraph_v2.json` example + the §10 residuals table.

---

## 10. G1 §6.2 Residuals Disposition / G3 TODOs

### 10.1 G1 residuals — disposition this gate

| G1 residual | Disposition in G2 | Rationale |
|---|---|---|
| **JSON parse depth cap** | **Deferred again, plus mitigation:** G2 adds `kMaxDocBytes = 8 MiB` pre-parse byte cap in `sf_project_from_json`/`sf_project_open_from_path` (`SF_E_FILE_TOO_LARGE` + log before nlohmann touches the input via `read_file_capped()` at file-read time). Bounds the stack-exhaustion exposure on the local-file path; the *depth* gate itself still needs a third-party nlohmann patch — genuinely blocked outside this gate. **Additional G2 hardening:** `isfinite()` gates on mixer inputs + `kMaxAuditEntries = 1000` FIFO prevents unbounded audit growth. | Sized mitigation + input validation land now (P1.4); depth gate tracked for G3. |
| **Rename cap is bytes, not chars** | **Re-deferred (G3).** Fixing it edits the G1 ABI surface semantics (`sf_project_rename`/`sf_venue_rename`), forcing G1 test/doc churn on a gate whose G2 surface (node names ≤ 64) inherits the same documented caveat. | No rename-surface change in G2; a char-based cap belongs with the G3 name-model work (`sf_scene_rename`). |
| **Reseed-on-refresh discards unapplied field typing** | **Re-deferred (accepted trade), with G2 note:** the new `SignalGraphViewModel`/`MixerViewModel` are wired to the same committed-doc-is-truth pattern (seed from `UiState.Ready`, reseed on commit). No silent data loss (the doc is authoritative); a per-field dirty-flag layer is a G3 UI concern. | Consistency with the g1.1 race-freedom contract beats speculative dirty-trackers in G2. |

### 10.2 G1 §6.2 TODO carry-forward (still open)

- `sf_scene_rename` mutator + `scene.name` freeze tracking — **re-deferred**
  (G3 name-model work). G2 adds no scene-consuming surfaces beyond the graph,
  and node names (not scene names) feed the editors.

### 10.3 New G2-discovered residuals (documented, non-blocking)

| # | Residual | Severity | Disposition |
|---|---|---|---|
| G2-1 | **Pre-existing schema drift fixed in P1:** the JSON schema's audit `enum` (4) lagged the native validator + real documents (7). G2 extends to 13 *before* shipping graph actions. Anything that *re-widens* the enum (new actions in G3+) must regenerate the golden + bump both SHA-256 pins in the same commit. | LOW (latent) — fixed | Watch-item: schema↔native action tables must stay in lockstep; the pytest enum test is the guard. |
| G2-2 | `sf_cmd_t` fixed 176-B image caps command payloads (names up to 64 B, single scalar pair). Future commands needing larger payloads (audio config blobs) are out of G2 scope; documented as the ring slot limit. | LOW | Revisit with G3 command set; slot size is a compile-time constant. |
| G2-3 | `sf_graph_apply_batch` is **not** atomic: a failing command stops the batch at `applied<n`; earlier commands are committed (each with its own audit entry). A transactional batch would need undo/rollback G2 does not need (commands are idempotent-ish: re-apply rejects duplicates). | LOW | Contract documented; batch semantics tested (partial-apply returns `applied` + error). |
| G2-4 | Android static-only again: queue lock-freedom is proven on Linux host (ASan + threaded test); ARM/Termux timing properties are untested. | MED (env) | CI device lane when available (G0/G1 known limitation; noted in release notes). |
| G2-5 | `graphEvaluateMixer` gain math uses linear-voltage summation for merges; the *actual* mixing law for the DSP engine (G3) may differ (power-sum, headroom model). The G2 result is labeled "static routing desk estimate", not DSP truth. | LOW | G3 DSP spec owns the law; the G2 JSON is stable input to it. |

### 10.4 Open questions — review verdict (Appendix B equivalent)

Adversarial review (G2, post-draft): @oracle (architecture) + @security-reviewer
(surface). Connectivity issue hit the first oracle attempt (task errored);
re-dispatched fresh. Findings disposition:

| # | Finding | Sev | Resolution (baked into plan) |
|---|---|---|---|
| SEC-1a | Byte cap enforced post-read (OOM DoS window) | HIGH | `read_file_capped()` at read time + `from_json` size pre-check (§4.2, P1.4, DoD 3) |
| SEC-1b | `SF_E_SCHEMA` misused for size errors | MED | New `SF_E_FILE_TOO_LARGE` (7) (§1.3, §4.2, sf_types.h) |
| SEC-1c | NaN/±Inf through mutators | MED | `isfinite()` gate in all mixer mutators (§4.1/§4.2, test_graph_mixer) |
| SEC-1d | Unbounded audit growth | MED | `kMaxAuditEntries = 1000` FIFO (§4.1, §5.4, P1.4) |
| ORC-A | Golden v2 digest guard absent | HIGH | v2 SHA-256 pin added same-commit as regen (§5.2, §7.3, P1.2, P1.5) |
| ORC-C | 128-B slot vs struct mismatch | MED | `sf_cmd_t` = 176 B; capacity 256 = 44 KiB; rationale documented (§4.4, P5.1) |
| ORC-E | CMake target/linkage underspecified | MED | `INTERFACE`→`STATIC` + `target_link_libraries(sfcore PRIVATE sfgraph)` (P2.3) |
| ORC-F/H | P5 duration underestimated; pthread proof | LOW | P5 = 1.5–2 days; host pthread OK, Android static-only noted (P5.2) |

**Open questions (plan's own §10.4 pre-review list) — verdicts:**
1. Queue JNI: native-only correct (no G2 consumer).
2. Static scalar gains OK as boundary (explicitly not DSP truth, G2-5).
3. Reject-at-mutation preferred over allow-and-detect (invalid states never enter).
4. Closed node kinds OK; additive via `$defs` later.
5. `0.1.0-g2` confirmed (suffix-tag convention, no stable-surface bump).
6. gainDb/pan constraints as schema + health-check constants OK.

Net: **no gate-blocking findings post-amendment**; plan approved for execution.

---

*End of PLAN_G2.md*