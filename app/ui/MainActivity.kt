// SoundForge G0 — single activity hosting the Compose shell.
package id.soundforge.pastudio

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import id.soundforge.pastudio.navigation.SoundForgeNavGraph
import id.soundforge.pastudio.theme.SoundForgeTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            SoundForgeTheme {
                SoundForgeNavGraph()
            }
        }
    }
}
