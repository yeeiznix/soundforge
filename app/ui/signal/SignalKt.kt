// SoundForge G2 — typed mirror of the project document's signalGraph object
// (docs/PLAN_G2.md §4.6). Immutable read-only view over opaque native JSON,
// parsed from NativeBridge.projectToJson output on the IO dispatcher — same
// posture as SceneKt (G1). Editing goes through SignalGraphViewModel /
// MixerViewModel, never by mutating these values.
//
// JSON shape mirrored here (project_v2.json $defs.signalNode / signalEdge):
//   signalGraph: { nodes: [ { kind: "source"|"processor"|"output"|"bus",
//                            id, label, position: {x, y}, mixer: {mute, solo} } ],
//                  edges: [ { from, to, label } ] }
// The v2 wire intentionally omits gainDb/pan, dspPresetRef, and edge ids/ports
// (PLAN_G2 §4.2) — those fields parse to defaults and are tracked locally by
// the ViewModels; committed-doc-is-truth still holds for everything on the wire.
package id.soundforge.pastudio.signal

import org.json.JSONObject

/** Node kinds, mirroring SF_NODE_* in sf_graph.h (1..4). */
const val SF_NODE_SOURCE = 1
const val SF_NODE_PROCESSOR = 2
const val SF_NODE_OUTPUT = 3
const val SF_NODE_BUS = 4

/** Wire kind-name -> numeric kind; tolerant (unknown/missing -> source). */
fun signalNodeKindOf(name: String?): Int = when (name) {
    "processor" -> SF_NODE_PROCESSOR
    "output" -> SF_NODE_OUTPUT
    "bus" -> SF_NODE_BUS
    else -> SF_NODE_SOURCE
}

/** Numeric kind -> wire kind-name (used by the kind picker + graphAddNode). */
fun signalNodeKindName(kind: Int): String = when (kind) {
    SF_NODE_PROCESSOR -> "processor"
    SF_NODE_OUTPUT -> "output"
    SF_NODE_BUS -> "bus"
    else -> "source"
}

/** Read-only signal-node snapshot. gainDb/pan/dspPresetRef are NOT on the v2
 *  wire (they are mixer-setter inputs); they default and the ViewModels track
 *  the latest committed values locally. */
data class SignalNodeKt(
    val id: String,
    val kind: Int,          // SF_NODE_* (1..4)
    val name: String,       // wire "label"
    val x: Double,          // wire position.x
    val y: Double,          // wire position.y
    val gainDb: Double = 0.0,   // not on the v2 wire (mixer setter value)
    val pan: Double = 0.0,      // not on the v2 wire
    val mute: Boolean,          // wire mixer.mute
    val solo: Boolean,          // wire mixer.solo
    val dspPresetRef: String = "", // not on the v2 wire; "" = none
)

/** Read-only signal-edge snapshot. Edge ids and ports are NOT on the v2 wire;
 *  they default ("" / 0) until P3+ documents carry them. */
data class SignalEdgeKt(
    val id: String = "",        // not on the v2 wire
    val fromNodeId: String,     // wire "from"
    val toNodeId: String,       // wire "to"
    val fromPort: Int = 0,      // not on the v2 wire
    val toPort: Int = 0,        // not on the v2 wire
)

/** Read-only signal-graph snapshot (committed document). */
data class SignalGraphKt(
    val nodes: List<SignalNodeKt>,
    val edges: List<SignalEdgeKt>,
) {
    val isEmpty: Boolean get() = nodes.isEmpty() && edges.isEmpty()
}

/** Tolerant parse of the root project document's "signalGraph" object. */
fun projectSignalGraphOf(root: JSONObject): SignalGraphKt? = runCatching {
    val sg = root.getJSONObject("signalGraph")
    val nodes = sg.optJSONArray("nodes")
        ?.let { arr -> (0 until arr.length()).mapNotNull { i -> signalNodeOf(arr.optJSONObject(i)) } }
        ?: emptyList()
    val edges = sg.optJSONArray("edges")
        ?.let { arr -> (0 until arr.length()).mapNotNull { i -> signalEdgeOf(arr.optJSONObject(i)) } }
        ?: emptyList()
    SignalGraphKt(nodes = nodes, edges = edges)
}.getOrNull()

private fun signalNodeOf(node: JSONObject): SignalNodeKt? = runCatching {
    val mixer = node.optJSONObject("mixer")
    val position = node.optJSONObject("position")
    SignalNodeKt(
        id = node.optString("id", ""),
        kind = signalNodeKindOf(node.optString("kind", "source")),
        name = node.optString("label", ""),
        x = position?.optDouble("x", 0.0) ?: 0.0,
        y = position?.optDouble("y", 0.0) ?: 0.0,
        mute = mixer?.optBoolean("mute", false) ?: false,
        solo = mixer?.optBoolean("solo", false) ?: false,
    )
}.getOrNull()

private fun signalEdgeOf(edge: JSONObject): SignalEdgeKt? = runCatching {
    SignalEdgeKt(
        fromNodeId = edge.optString("from", ""),
        toNodeId = edge.optString("to", ""),
    )
}.getOrNull()