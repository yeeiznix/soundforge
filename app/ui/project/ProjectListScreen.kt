// SoundForge G0 — saved project list: open via tap, soft-delete via long-press.
package id.soundforge.pastudio.project

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import id.soundforge.pastudio.platform.storage.FileStorageRepository
import id.soundforge.pastudio.platform.storage.ProjectFile
import java.io.File
import kotlinx.coroutines.launch

/**
 * Lists saved .sfproj documents from the app's filesDir/projects folder.
 *
 * Tap: read the file (fail fast on missing paths) and hand off to the shared
 * [ProjectViewModel.openFromPath], then navigate back home. Long-press: G0
 * soft delete — the entry is removed from UI state only, no file is erased.
 */
@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun ProjectListScreen(
    viewModel: ProjectViewModel,
    onNavigateBack: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val repo = remember { FileStorageRepository(File(context.filesDir)) }
    var projects by remember { mutableStateOf<List<ProjectFile>>(emptyList()) }
    var showCreateDialog by remember { mutableStateOf(false) }

    LaunchedEffect(repo) {
        projects = repo.listProjects()
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(text = "Projects") },
                navigationIcon = {
                    IconButton(onClick = onNavigateBack) {
                        Icon(imageVector = Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = { showCreateDialog = true }) {
                Text(text = "+")
            }
        },
    ) { padding ->
        if (projects.isEmpty()) {
            Box(
                modifier = Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center,
            ) {
                Text(text = "No projects yet")
            }
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize().padding(padding),
                contentPadding = PaddingValues(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                items(items = projects, key = { it.path }) { project ->
                    Text(
                        text = project.name,
                        style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier
                            .fillMaxWidth()
                            .combinedClickable(
                                onClick = {
                                    scope.launch {
                                        // Read first to fail fast; openFromPath re-reads via the repo.
                                        val bytes = repo.readProject(project.path).getOrNull()
                                            ?: return@launch
                                        viewModel.openFromPath(project.path)
                                        onNavigateBack()
                                    }
                                },
                                onLongClick = {
                                    // G0 soft delete: UI state only; the file stays on disk.
                                    projects = projects.filterNot { it.path == project.path }
                                },
                            ),
                    )
                }
            }
        }
    }

    if (showCreateDialog) {
        NewProjectDialog(
            onConfirm = { name, venuePreset ->
                showCreateDialog = false
                viewModel.create(name, venuePreset)
                onNavigateBack()
            },
            onDismiss = { showCreateDialog = false },
        )
    }
}
