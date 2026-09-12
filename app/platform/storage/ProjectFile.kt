// SoundForge G0 — lightweight listing record + filename sanitizer.
package id.soundforge.pastudio.platform.storage

/**
 * UI-facing descriptor of a stored project document.
 *
 * [schemaVersion]/[engineVersion] describe the writer of the file; listers
 * that do not parse document headers may report 0 / "" (see G0 repo).
 */
data class ProjectFile(
    val path: String,
    val name: String,
    val modifiedAt: Long,
    val schemaVersion: Int,
    val engineVersion: String,
)

/**
 * Map an arbitrary user-entered project name to a safe file stem.
 *
 * Keeps only [A-Za-z0-9_-]; every other character collapses to '_'. Returns
 * "project" when nothing usable remains.
 */
fun sanitizeFileName(name: String): String {
    val cleaned = buildString {
        for (c in name.trim()) {
            val keep = c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '_' || c == '-'
            append(if (keep) c else '_')
        }
    }
    return cleaned.ifBlank { "project" }
}
