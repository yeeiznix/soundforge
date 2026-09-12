// SoundForge G0 — placeholder: measurement flow arrives in a later milestone.
package id.soundforge.pastudio.measurement

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier

/** G0 placeholder shell for the measurement route. */
@Composable
fun MeasurementScreen(projectId: String?) {
    Scaffold { padding ->
        Box(
            modifier = Modifier.fillMaxSize().padding(padding),
            contentAlignment = Alignment.Center,
        ) {
            Text(text = "G0 placeholder — Measurement" + (projectId?.let { " (projectId=$it)" } ?: ""))
        }
    }
}
