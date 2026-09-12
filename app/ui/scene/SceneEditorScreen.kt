// SoundForge G1 — scene editor: scene name, venue picker, room-bounds geometry
// (docs/PLAN_G1.md §6.2). A pure view: NavGraph wires the commit lambdas to
// ProjectViewModel helpers (all on the shared IO dispatcher); the form re-seeds
// from the committed document on entry, and the native error message arrives
// via the [errorMessage] state slot on failed commits.
package id.soundforge.pastudio.scene

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenu
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.keyboard.KeyboardType
import androidx.compose.ui.unit.dp
import id.soundforge.pastudio.venue.VenueKt

/** Numeric-decimal TextField; the engine validates finite, >0 dimensions. */
@Composable
private fun numericField(
    label: String,
    value: String,
    onValueChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        label = { Text(label) },
        modifier = Modifier.fillMaxWidth(),
    )
}

/** Best-effort "12.0" formatter for seeding numeric fields. */
private fun fmt(v: Double): String {
    val s = v.toString()
    return if (s.endsWith(".0")) s.substring(0, s.length() - 2) else s
}

/** Tolerant parse; null on empty or malformed input (field stays amber). */
private fun parseDim(s: String): Double? = runCatching {
    val t = s.trim().replace(',', '.')
    if (t.isEmpty()) null else Double.parseDouble(t)
}.getOrNull()

/**
 * G1 scene editor. [projectId] may be null (no project open → affordance).
 * One project = one scene in v2; the editor commits geometry as a whole.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SceneEditorScreen(
    projectId: String?,
    projectHandle: Long = 0L,
    scene: SceneKt? = null,
    venue: VenueKt? = null,
    projectName: String = "",
    errorMessage: String? = null,
    onEditVenue: (String?) -> Unit = {},
    onNavigateBack: () -> Unit = {},
    onRenameScene: (String) -> Unit = {},
    onUpdateGeometry: (Pt3, Pt3) -> Unit = {},
) {
    if (projectId == null) {
        Scaffold { padding ->
            Box(
                modifier = Modifier.fillMaxSize().padding(padding),
                contentAlignment = Alignment.Center,
            ) {
                Text(text = "No project open — create or open one first")
            }
        }
        return
    }

    // --- Editable fields (seeded per document refresh) ---------------------
    var sceneName by remember { mutableStateOf("") }
    var cx by remember { mutableStateOf("") }
    var cy by remember { mutableStateOf("") }
    var cz by remember { mutableStateOf("") }
    var lx by remember { mutableStateOf("") }
    var ly by remember { mutableStateOf("") }
    var lz by remember { mutableStateOf("") }
    var errorText by remember { mutableStateOf<String?>(null) }
    var venueExpanded by remember { mutableStateOf(false) }

    LaunchedEffect(scene, projectName) {
        sceneName = projectName.ifEmpty { scene?.name ?: "" }
        val c = scene?.center ?: Pt3(0.0, 0.0, 0.0)
        val l = scene?.listening ?: Pt3(0.0, 0.0, 0.0)
        cx = fmt(c.x); cy = fmt(c.y); cz = fmt(c.z)
        lx = fmt(l.x); ly = fmt(l.y); lz = fmt(l.z)
        errorText = null
    }

    fun commitGeometry() {
        errorText = null
        val cxp = parseDim(cx); val cyp = parseDim(cy); val czp = parseDim(cz)
        val lxp = parseDim(lx); val lyp = parseDim(ly); val lzp = parseDim(lz)
        if (cxp == null || cyp == null || czp == null ||
            lxp == null || lyp == null || lzp == null
        ) {
            errorText = "Enter numbers for all six coordinates"
            return
        }
        // Commit lambda runs on the VM's IO dispatcher; the VM refreshes the
        // document on success and surfaces the native message via UiState.Error
        // (NavGraph feeds it back through [errorMessage]). Local field state
        // keeps the typed values until the next document load re-seeds.
        onUpdateGeometry(Pt3(cxp, cyp, czp), Pt3(lxp, lyp, lzp))
    }

    fun commitName() {
        val name = sceneName.trim()
        if (name.isEmpty() || name == projectName) return
        errorText = null
        onRenameScene(name)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(text = "Scene editor — ${projectName.ifEmpty { "…" }}") },
                navigationIcon = {
                    IconButton(onClick = onNavigateBack) {
                        Icon(imageVector = Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            val s = scene
            val v = venue
            if (s == null || v == null) {
                Text(text = "No scene/venue data in the open project")
                return@Column
            }

            // Edits the PROJECT name (single-name model PLAN_G1 §6.2).
            OutlinedTextField(
                value = sceneName,
                onValueChange = { sceneName = it },
                singleLine = true,
                label = { Text(text = "Scene/project name") },
                modifier = Modifier.fillMaxWidth(),
            )
            TextButton(
                onClick = commitName,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Rename scene") }
            if (errorMessage != null) {
                Text(
                    text = errorMessage,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            ExposedDropdownMenuBox(
                expanded = venueExpanded,
                onExpandedChange = { venueExpanded = it },
            ) {
                OutlinedTextField(
                    value = v.name,
                    onValueChange = {},
                    readOnly = true,
                    label = { Text(text = "Venue") },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = venueExpanded) },
                    modifier = Modifier.menuAnchor().fillMaxWidth(),
                )
                ExposedDropdownMenu(
                    expanded = venueExpanded,
                    onDismissRequest = { venueExpanded = false },
                ) {
                    DropdownMenuItem(
                        text = { Text(text = v.name) },
                        onClick = { venueExpanded = false },
                    )
                    DropdownMenuItem(
                        text = { Text(text = "Edit venue…") },
                        onClick = {
                            venueExpanded = false
                            onEditVenue(projectId)
                        },
                    )
                }
            }

            Text(
                text = "Geometry (meters)",
                style = MaterialTheme.typography.titleSmall,
            )
            Row {
                numericField("Center X", cx) { cx = it }
                numericField("Center Y", cy) { cy = it }
                numericField("Center Z", cz) { cz = it }
            }
            Row {
                numericField("Listening X", lx) { lx = it }
                numericField("Listening Y", ly) { ly = it }
                numericField("Listening Z", lz) { lz = it }
            }
            if (errorText != null) {
                Text(
                    text = errorText ?: "",
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
            Button(
                onClick = commitGeometry,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Apply scene geometry") }
        }
    }
}