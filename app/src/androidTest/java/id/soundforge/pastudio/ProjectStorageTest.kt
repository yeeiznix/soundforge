// SoundForge G0 — storage round-trip test (requires a device/emulator).
package id.soundforge.pastudio

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import id.soundforge.pastudio.platform.bridge.NativeBridge
import id.soundforge.pastudio.platform.storage.FileStorageRepository
import java.io.File
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Round-trip: NativeBridge.projectCreate -> projectToJson -> FileStorage
 * write -> listProjects -> readProject, with byte equality on the way back.
 *
 * NOTE: this test uses NativeBridge and therefore only runs where
 * libsfcore.so is packaged (real device/emulator). Host JVM runs must use
 * NavigationSmokeTest, which avoids native calls entirely.
 */
@RunWith(AndroidJUnit4::class)
class ProjectStorageTest {

    private val context: Context = ApplicationProvider.getApplicationContext()
    private lateinit var repo: FileStorageRepository

    @Before
    fun setUp() {
        // Fresh slate under filesDir/projects for every run.
        File(context.filesDir, "projects").deleteRecursively()
        repo = FileStorageRepository(File(context.filesDir))
    }

    @Test
    fun createSerializeWriteListRead_roundtrip() {
        val handle = NativeBridge.projectCreate("StorageTest", null)
        assertNotEquals("projectCreate failed", 0L, handle)
        try {
            val json = NativeBridge.projectToJson(handle)
            assertTrue("projectToJson returned empty", json.isNotEmpty())
            val bytes = json.toByteArray(Charsets.UTF_8)

            val path = File(context.filesDir, "projects/storage_test.sfproj").absolutePath
            assertTrue("writeProject failed", runBlocking { repo.writeProject(path, bytes) }.isSuccess)

            val listed = runBlocking { repo.listProjects() }
            assertEquals("expected exactly one listed project", 1, listed.size)

            val read = runBlocking { repo.readProject(path) }
            assertTrue("readProject failed", read.isSuccess)
            assertEquals("round-trip bytes differ", bytes.toList(), read.getOrThrow().toList())
        } finally {
            NativeBridge.projectDestroy(handle)
        }
    }
}
