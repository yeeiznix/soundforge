// SoundForge G1 — scene editor: scene name, venue picker, room-bounds geometry
// (docs/PLAN_G1.md §6.2). A pure view: NavGraph wires the commit lambdas to
// ProjectViewModel helpers (all on the shared IO dispatcher); the form re-seeds
// from the committed document on entry, and the native error message arrives
// via the [errorMessage] state slot on failed commits.
// G3 §6 P6: a distinct "Scene name" field renames the scene OBJECT
// (sf_scene_rename via ProjectViewModel.sceneRename), while the existing
// "Scene/project name" field keeps renaming the PROJECT (unchanged behavior).
// The new field is a DirtyField: its commit button enables only while the
// field is dirty and re-seeds never clobber an in-progress edit (D7).
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import id.soundforge.pastudio.common.DirtyField
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
    return if (s.endsWith(".0")) s.substring(0, s.length - 2) else s
}

/** Tolerant parse; null on empty or malformed input (field stays amber). */
private fun parseDim(s: String): Double? = runCatching {
    val t = s.trim().replace(',', '.')
    if (t.isEmpty()) null else t.toDoubleOrNull()
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
    /** Committed scene-object name (UiState.Ready.scene.name); seeds the
     *  G3 "Scene name" DirtyField and never clobbers a dirty edit. */
    sceneName: String = "",
    errorMessage: String? = null,
    onEditVenue: (String?) -> Unit = {},
    onNavigateBack: () -> Unit = {},
    onRenameScene: (String) -> Unit = {},
    /** G3: renames the scene OBJECT via ProjectViewModel.sceneRename
     *  (sf_scene_rename) — distinct from [onRenameScene] (project rename). */
    onRenameSceneName: (String) -> Unit = {},
    onUpdateGeometry: (Pt3, Pt3) -> Unit = { _: Pt3, _: Pt3 -> },
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
    var projectNameField by remember { mutableStateOf("") }
    var cx by remember { mutableStateOf("") }
    var cy by remember { mutableStateOf("") }
    var cz by remember { mutableStateOf("") }
    var lx by remember { mutableStateOf("") }
    var ly by remember { mutableStateOf("") }
    var lz by remember { mutableStateOf("") }
    var errorText by remember { mutableStateOf<String?>(null) }
    var venueExpanded by remember { mutableStateOf(false) }

    // G3 P6: scene object name, DirtyField-tracked (seed/commit/isDirty).
    val sceneNameField = remember { DirtyField(sceneName) }
    // Re-seed from the committed document — only while pristine.
    LaunchedEffect(sceneName) { sceneNameField.seed(sceneName) }

    LaunchedEffect(scene, projectName) {
        projectNameField = projectName.ifEmpty { scene?.name ?: "" }
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
        val name = projectNameField.trim()
        if (name.isEmpty() || name == projectName) return
        errorText = null
        onRenameScene(name)
    }

    fun commitSceneName() {
        val name = sceneNameField.value.trim()
        if (name.isEmpty()) return
        errorText = null
        onRenameSceneName(name)
        sceneNameField.commit()
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

            // G3 P6: the scene OBJECT's own name — renames doc.scene.name via
            // sf_scene_rename. Commit button gated on DirtyField.isDirty.
            OutlinedTextField(
                value = sceneNameField.value,
                onValueChange = { sceneNameField.edit(it) },
                singleLine = true,
                label = { Text(text = "Scene name") },
                modifier = Modifier.fillMaxWidth(),
            )
            TextButton(
                onClick = { commitSceneName() },
                enabled = sceneNameField.isDirty,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Apply scene name") }

            // Edits the PROJECT name (single-name model PLAN_G1 §6.2) — G1
            // behavior, label and commit path preserved unchanged.
            OutlinedTextField(
                value = projectNameField,
                onValueChange = { projectNameField = it },
                singleLine = true,
                label = { Text(text = "Scene/project name") },
                modifier = Modifier.fillMaxWidth(),
            )
            TextButton(
                onClick = { commitName() },
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
                onClick = { commitGeometry() },
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Apply scene geometry") }
        }
    }
}