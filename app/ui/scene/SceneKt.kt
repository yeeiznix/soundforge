// SoundForge G1 — typed mirror of the v2 project document's scene object
// (docs/PLAN_G1.md §6.1). Immutable read-only view over opaque native JSON;
// parsed from NativeBridge.projectToJson output on the IO dispatcher, same
// posture as ProjectMetaKt. Editing goes through ProjectViewModel helpers,
// never by mutating these values.
//
// JSON shape mirrored here (project_v2.json $defs.point3):
//   scene: { id, name, venueRef, geometry: { center: point3, listening: point3 } }
//   point3: { x, y, z }
package id.soundforge.pastudio.scene

/** A 3-D point in meters; shared by scene geometry and the venue editor. */
data class Pt3(
    val x: Double,
    val y: Double,
    val z: Double,
)

/** Read-only scene snapshot (singleton venue; venueRef names the venue). */
data class SceneKt(
    val id: String,
    val name: String,
    val venueRef: String,
    val center: Pt3,
    val listening: Pt3,
)