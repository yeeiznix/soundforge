// SoundForge G0 — repository contract for project documents on disk.
package id.soundforge.pastudio.platform.storage

/**
 * Storage abstraction over saved .sfproj project files.
 *
 * All methods are suspending; implementations must not block the caller.
 * [readProject] and [writeProject] take absolute filesystem paths.
 */
interface StorageRepository {

    /** List known project documents, newest first. */
    suspend fun listProjects(): List<ProjectFile>

    /** Read the raw bytes of a project document at [path]. */
    suspend fun readProject(path: String): Result<ByteArray>

    /** Write raw project bytes to [path] (creating parent directories). */
    suspend fun writeProject(path: String, bytes: ByteArray): Result<Unit>
}
