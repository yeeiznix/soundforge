// SoundForge G0 — home dashboard: engine identity, diagnostics, entry points.
package id.soundforge.pastudio.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import id.soundforge.pastudio.platform.bridge.NativeBridge
import id.soundforge.pastudio.platform.diagnostics.SfLogger
import id.soundforge.pastudio.platform.storage.FileStorageRepository
import id.soundforge.pastudio.platform.storage.ProjectFile
import id.soundforge.pastudio.project.NewProjectDialog
import java.io.File

/** Number of recent projects surfaced on the dashboard. */
private const val RECENT_LIMIT = 5

/**
 * Home dashboard (start destination).
 *
 * Pure composable: [NavGraph][id.soundforge.pastudio.navigation.SoundForgeNavGraph]
 * hoists the shared ProjectViewModel and forwards the callbacks. Native
 * version reads are wrapped in runCatching so the shell still renders when
 * libsfcore.so is absent (host instrumentation runs).
 */
@Composable
fun HomeScreen(
    onCreateProject: (String, String) -> Unit,
    onOpenProjectList: () -> Unit,
    openProjectName: String? = null,
) {
    val context = LocalContext.current
    var showCreateDialog by remember { mutableStateOf(false) }
    var recent by remember { mutableStateOf<List<ProjectFile>>(emptyList()) }
    val repo = remember { FileStorageRepository(context.filesDir) }

    // Read engine identity once per composition; tolerate a missing native lib.
    val engineInfo = remember {
        runCatching { NativeBridge.engineVersion() to NativeBridge.schemaVersion() }.getOrNull()
    }

    LaunchedEffect(repo) {
        recent = repo.listProjects().take(RECENT_LIMIT)
    }

    Scaffold { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            AssistChip(
                onClick = {},
                label = {
                    Text(
                        text = engineInfo
                            ?.let { (version, schema) -> "Engine $version / Schema $schema" }
                            ?: "Engine unknown / Schema unknown",
                    )
                },
            )
            AssistChip(
                onClick = {},
                label = {
                    Text(
                        text = if (SfLogger.lastProbeReport != null) "Diagnostics ready"
                        else "Diagnostics pending",
                    )
                },
            )
            if (openProjectName != null) {
                Text(
                    text = "Open project: $openProjectName",
                    style = MaterialTheme.typography.titleMedium,
                )
            }
            Button(
                onClick = { showCreateDialog = true },
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Create Project") }
            Button(
                onClick = onOpenProjectList,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Open Project") }

            Text(text = "Recent projects", style = MaterialTheme.typography.titleSmall)
            if (recent.isEmpty()) {
                Text(text = "No saved projects yet")
            } else {
                recent.forEach { project ->
                    Text(text = "\u2022 ${project.name}")
                }
            }
        }
    }

    if (showCreateDialog) {
        NewProjectDialog(
            onConfirm = { name, venuePreset ->
                showCreateDialog = false
                onCreateProject(name, venuePreset)
            },
            onDismiss = { showCreateDialog = false },
        )
    }
}
