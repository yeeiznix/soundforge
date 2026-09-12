// SoundForge G1 — venue editor: name and room dimensions (meters).
// (docs/PLAN_G1.md §6.3). A pure view: NavGraph wires the commit lambda to
// ProjectViewModel helpers (all on the shared IO dispatcher); the form re-seeds
// from the committed document on entry, and the native error message arrives
// via the [errorMessage] state slot on failed commits.
package id.soundforge.pastudio.venue

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
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
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.keyboard.KeyboardType
import androidx.compose.ui.unit.dp
import id.soundforge.pastudio.platform.bridge.NativeBridge
import org.json.JSONObject

/** Numeric-decimal TextField; the engine validates finite, >0 dimensions. */
@Composable
private fun venueNumericField(
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

/** Tolerant parse; null on empty or malformed input. */
private fun parseDim(s: String): Double? = runCatching {
    val t = s.trim().replace(',', '.')
    if (t.isEmpty()) null else Double.parseDouble(t)
}.getOrNull()

/**
 * G1 venue editor. [projectId] may be null (no project open → affordance).
 * The project has exactly one venue in v2; edits commit name + dimensions
 * together so the native audit trail shows one "venue.update" entry.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun VenueScreen(
    projectId: String?,
    projectHandle: Long = 0L,
    errorMessage: String? = null,
    onUpdateVenue: (String, Double, Double, Double) -> Unit = { _: String, _: Double, _: Double, _: Double -> },
    onNavigateBack: () -> Unit = {},
) {
    if (projectId == null) {
        Scaffold { padding ->
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding)
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(text = "No project open — create or open one first")
            }
        }
        return
    }

    // --- Document snapshot (loaded on entry / handle change) ----------------
    var venue by remember { mutableStateOf<VenueKt?>(null) }
    var docError by remember { mutableStateOf<String?>(null) }

    fun reloadDocument() {
        docError = null
        val json = NativeBridge.projectToJson(projectHandle)
        if (json.isEmpty()) {
            docError = NativeBridge.lastError(projectHandle)
            return
        }
        venue = runCatching { JSONObject(json) }.getOrNull()?.let { projectVenueOf(it) }
    }

    LaunchedEffect(projectHandle) { reloadDocument() }

    // --- Editable fields (seeded per document refresh) ---------------------
    var venueName by remember { mutableStateOf("") }
    var w by remember { mutableStateOf("") }
    var d by remember { mutableStateOf("") }
    var h by remember { mutableStateOf("") }
    var errorText by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(venue) {
        venueName = venue?.name ?: ""
        w = fmt(venue?.widthM ?: 0.0)
        d = fmt(venue?.depthM ?: 0.0)
        h = fmt(venue?.heightM ?: 0.0)
        errorText = null
    }

    fun commit() {
        errorText = null
        val name = venueName.trim()
        if (name.isEmpty()) {
            errorText = "Venue name must be non-empty"
            return
        }
        val wp = parseDim(w); val dp = parseDim(d); val hp = parseDim(h)
        if (wp == null || dp == null || hp == null) {
            errorText = "Enter numbers for all three dimensions"
            return
        }
        // Commit lambda runs on the VM's IO dispatcher; the VM refreshes the
        // document on success and surfaces the native message via UiState.Error
        // (NavGraph feeds it back through [errorMessage]).
        onUpdateVenue(name, wp, dp, hp)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(text = "Venue editor — ${venue?.name ?: "…"}") },
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
            if (docError != null) {
                Text(
                    text = docError ?: "",
                    style = MaterialTheme.typography.bodyMedium,
                )
                return@Column
            }
            if (venue == null) {
                Text(text = "No venue data in the open project")
                return@Column
            }

            OutlinedTextField(
                value = venueName,
                onValueChange = { venueName = it },
                singleLine = true,
                label = { Text(text = "Venue name") },
                modifier = Modifier.fillMaxWidth(),
            )
            venueNumericField("Width (m)", w) { w = it }
            venueNumericField("Depth (m)", d) { d = it }
            venueNumericField("Height (m)", h) { h = it }

            if (errorMessage != null) {
                Text(
                    text = errorMessage,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }
            if (errorText != null) {
                Text(
                    text = errorText ?: "",
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
            Button(
                onClick = commit,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Apply venue changes") }
            TextButton(
                onClick = onNavigateBack,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(text = "Back to scene") }
        }
    }
}