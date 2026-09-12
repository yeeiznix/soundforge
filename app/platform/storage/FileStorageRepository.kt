// SoundForge G0 — filesystem-backed StorageRepository (shell-level IO only).
//
// Durable, atomic persistence lives in the native core
// (sf_project_save_to_path: write-to-temp + rename). This class does simple
// byte reads/writes for the G0 shell and is intentionally non-atomic.
package id.soundforge.pastudio.platform.storage

import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** [StorageRepository] rooted at [baseDir]/projects, listing *.sfproj files. */
class FileStorageRepository(baseDir: File) : StorageRepository {

    private val projectsDir = File(baseDir, "projects")

    override suspend fun listProjects(): List<ProjectFile> = withContext(Dispatchers.IO) {
        val files = projectsDir
            .takeIf { it.isDirectory }
            ?.listFiles { candidate -> candidate.isFile && candidate.name.endsWith(PROJECT_EXTENSION) }
            .orEmpty()
        files
            .sortedByDescending { it.lastModified() }
            .map { file ->
                // G0: listing does not parse document headers; version fields stay blank.
                ProjectFile(
                    path = file.absolutePath,
                    name = file.nameWithoutExtension,
                    modifiedAt = file.lastModified(),
                    schemaVersion = 0,
                    engineVersion = "",
                )
            }
    }

    override suspend fun readProject(path: String): Result<ByteArray> = withContext(Dispatchers.IO) {
        runCatching {
            val file = File(path)
            check(file.isFile) { "Project file not found: $path" }
            FileInputStream(file).use { it.readBytes() }
        }
    }

    override suspend fun writeProject(path: String, bytes: ByteArray): Result<Unit> =
        withContext(Dispatchers.IO) {
            runCatching {
                val file = File(path)
                file.parentFile?.mkdirs()
                FileOutputStream(file).use { it.write(bytes) }
            }
        }

    private companion object {
        const val PROJECT_EXTENSION = ".sfproj"
    }
}
