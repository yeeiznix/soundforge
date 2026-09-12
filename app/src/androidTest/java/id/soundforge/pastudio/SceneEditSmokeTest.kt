// SoundForge G1 — scene/venue editor smoke test (host-safe, no native calls).
package id.soundforge.pastudio

import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.test.ext.junit.runners.AndroidJUnit4
import id.soundforge.pastudio.scene.SceneEditorScreen
import id.soundforge.pastudio.venue.VenueScreen
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * G1 editor smoke test.
 *
 * HOST RUN LIMITATION: libsfcore.so is absent in host JVM runs, so every
 * screen is composed with a null projectId — the editor renders its static
 * "no project open" affordance and never touches NativeBridge.
 * Edit interactions (which require a live native handle) are device-only.
 */
@RunWith(AndroidJUnit4::class)
class SceneEditSmokeTest {

    @get:Rule
    val composeRule = createComposeRule()

    @Test
    fun sceneEditor_showsNoProjectAffordance() {
        composeRule.setContent {
            SceneEditorScreen(projectId = null)
        }
        composeRule.onNodeWithText("No project open — create or open one first").assertExists()
    }

    @Test
    fun venueEditor_showsNoProjectAffordance() {
        composeRule.setContent {
            VenueScreen(projectId = null)
        }
        composeRule.onNodeWithText("No project open — create or open one first").assertExists()
    }
}