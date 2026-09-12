// SoundForge G1 — typed mirror of the v2 project document's venue object
// (docs/PLAN_G1.md §6.1). Immutable read-only view over opaque native JSON;
// parsed from NativeBridge.projectToJson output on the IO dispatcher, same
// posture as ProjectMetaKt. Editing goes through ProjectViewModel helpers,
// never by mutating these values.
//
// JSON shape mirrored here (project_v2.json, v2 §4.2):
//   venue: { id, name, dimensions: { widthM, depthM, heightM } }
// All dimensions are meters; > 0 and finite on a healthy document.
package id.soundforge.pastudio.venue

import id.soundforge.pastudio.scene.Pt3
import org.json.JSONObject

/** Read-only venue snapshot (the project has exactly one venue in v2). */
data class VenueKt(
    val id: String,
    val name: String,
    val widthM: Double,
    val depthM: Double,
    val heightM: Double,
)

/** Room-bounds helper for the scene editor: is [pt] inside [dimensions]? */
fun Pt3.withinRoomsOf(room: VenueKt): Boolean =
    x >= 0.0 && x <= room.widthM &&
        y >= 0.0 && y <= room.depthM &&
        z >= 0.0 && z <= room.heightM

/** Tolerant parse of the root project document's "venue" object. */
fun projectVenueOf(root: JSONObject): VenueKt? = runCatching {
    val venue = root.getJSONObject("venue")
    val dims = venue.getJSONObject("dimensions")
    VenueKt(
        id = venue.optString("id", ""),
        name = venue.optString("name", ""),
        widthM = dims.optDouble("widthM", 0.0),
        depthM = dims.optDouble("depthM", 0.0),
        heightM = dims.optDouble("heightM", 0.0),
    )
}.getOrNull()