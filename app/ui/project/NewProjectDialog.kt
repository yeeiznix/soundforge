// SoundForge G0 — project creation dialog (name + venue preset).
package id.soundforge.pastudio.project

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/** Venue presets offered at project creation (G0: captured, not persisted yet). */
private val VenuePresets = listOf("Small Club", "Warehouse", "Theater")

/**
 * Modal dialog for creating a project.
 *
 * [onConfirm] receives the trimmed project name; the selected venue preset is
 * G0 UI-only state (a later milestone persists it into the project document).
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun NewProjectDialog(
    onConfirm: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var name by remember { mutableStateOf("") }
    var venue by remember { mutableStateOf(VenuePresets.first()) }
    var venueExpanded by remember { mutableStateOf(false) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(text = "New Project") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    singleLine = true,
                    label = { Text(text = "Project name") },
                    modifier = Modifier.fillMaxWidth(),
                )
                ExposedDropdownMenuBox(
                    expanded = venueExpanded,
                    onExpandedChange = { venueExpanded = it },
                ) {
                    OutlinedTextField(
                        value = venue,
                        onValueChange = {},
                        readOnly = true,
                        label = { Text(text = "Venue preset") },
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = venueExpanded) },
                        modifier = Modifier.menuAnchor().fillMaxWidth(),
                    )
                    ExposedDropdownMenu(
                        expanded = venueExpanded,
                        onDismissRequest = { venueExpanded = false },
                    ) {
                        VenuePresets.forEach { preset ->
                            DropdownMenuItem(
                                text = { Text(text = preset) },
                                onClick = {
                                    venue = preset
                                    venueExpanded = false
                                },
                            )
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(name.trim()) },
                enabled = name.isNotBlank(),
            ) { Text(text = "Confirm") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(text = "Dismiss") }
        },
    )
}
