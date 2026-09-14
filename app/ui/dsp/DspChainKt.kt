// SoundForge G3 — typed mirror of the project document's dspPresets envelope
// collection (docs/PLAN_G3.md §4.6/§6 P6). Immutable read-only view over
// opaque native JSON, parsed from NativeBridge.projectToJson output on the IO
// dispatcher — same posture as SceneKt/SignalKt. The G3 editor attaches /
// replaces / clears node references over these EXISTING envelope ids only;
// `dspPresets[].data` stays unformalized (envelope contract only, no preset
// authoring — G3 §1.3).
//
// JSON shape mirrored here (objectEnvelope $defs):
//   dspPresets: [ { id, type, version, createdAt, modifiedAt, provenance,
//                   data: { ... unformalized ... } } ]
// Only id/type are guaranteed by the envelope contract; the display name is a
// best-effort peek at data.name and falls back to the raw id.
package id.soundforge.pastudio.dsp

import org.json.JSONObject

/** Read-only dspPreset envelope snapshot. */
data class DspPresetKt(
    val id: String,
    val type: String,
    /** Best-effort peek at the unformalized data.name; "" when absent. */
    val name: String = "",
) {
    /** Human-facing chip label: the data name when present, else the raw id. */
    val displayName: String get() = name.ifEmpty { id }
}

/** Tolerant parse of the root project document's "dspPresets" collection. */
fun projectDspPresetsOf(root: JSONObject): List<DspPresetKt> =
    root.optJSONArray("dspPresets")
        ?.let { arr -> (0 until arr.length()).mapNotNull { i -> dspPresetOf(arr.optJSONObject(i)) } }
        ?: emptyList()

private fun dspPresetOf(env: JSONObject): DspPresetKt? = runCatching {
    DspPresetKt(
        id = env.optString("id", ""),
        type = env.optString("type", "dspPreset"),
        name = env.optJSONObject("data")?.optString("name", "") ?: "",
    )
}.getOrNull()