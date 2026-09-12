// SoundForge G0 — navigation smoke test (host-safe, no native calls).
package id.soundforge.pastudio

import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onNodeWithText
import androidx.test.ext.junit.runners.AndroidJUnit4
import id.soundforge.pastudio.navigation.SoundForgeNavGraph
import id.soundforge.pastudio.theme.SoundForgeTheme
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/**
 * G0 shell smoke test.
 *
 * HOST RUN LIMITATION: libsfcore.so is absent in host JVM runs, so this test
 * deliberately avoids every NativeBridge-backed interaction path (version
 * reads in HomeScreen are wrapped in runCatching and degrade to "unknown").
 * Assertions are limited to statically rendered texts — the Create/Open
 * buttons. Tapping "Create Project" or opening a project would invoke the
 * native bridge and crash host runs; only extend assertions on a device.
 */
@RunWith(AndroidJUnit4::class)
class NavigationSmokeTest {

    @get:Rule
    val composeRule = createComposeRule()

    @Test
    fun home_showsPrimaryActions() {
        composeRule.setContent {
            SoundForgeTheme {
                SoundForgeNavGraph()
            }
        }
        composeRule.onNodeWithText("Create Project").assertExists()
        composeRule.onNodeWithText("Open Project").assertExists()
    }
}
