// SoundForge G2 — mixer: per-node gain/pan sliders + mute/solo toggles and a
// "Routing" preview card (docs/PLAN_G2.md §4.6/P7). A pure view: the channel
// set arrives via UiState.Ready.signalGraph and every slider/chip commit goes
// through the MixerViewModel callbacks wired here by NavGraph — no
// NativeBridge.* in this composable. As in SignalEditorScreen, native
// failures only set lastEditError (no emission), so commits poll it briefly;
// the document refresh after a success re-syncs the inline error.
package id.soundforge.pastudio.mixer

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AssistChip
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import id.soundforge.pastudio.signal.SF_NODE_BUS
import id.soundforge.pastudio.signal.SF_NODE_OUTPUT
import id.soundforge.pastudio.signal.SF_NODE_PROCESSOR
import id.soundforge.pastudio.signal.SignalGraphKt
import id.soundforge.pastudio.signal.SignalNodeKt
import kotlin.math.roundToInt
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject

/** Friendly display label for an SF_NODE_* kind. */
private fun kindLabel(kind: Int): String = when (kind) {
    SF_NODE_PROCESSOR -> "Processor"
    SF_NODE_OUTPUT -> "Output"
    SF_NODE_BUS -> "Bus"
    else -> "Source"
}

/** Pan (−1..1) as a readable balance label. */
private fun formatPan(pan: Float): String {
    val pct = (pan * 100).roundToInt()
    return when {
        pct < 0 -> "$pct% L"
        pct > 0 -> "$pct% R"
        else -> "center"
    }
}

/** Best-effort pretty print of the engine's Routing JSON. */
private fun prettyJson(raw: String): String = runCatching {
    JSONObject(raw).toString(2)
}.getOrDefault(raw)

/**
 * G2 mixer (PLAN_G2 §7.2). [projectId] null keeps the G0 "no project open"
 * affordance. Channels are signal-graph nodes; each strip commits via
 * [onSetMixer] on slider-release / chip tap. The pinned Routing card shows
 * graphEvaluateMixer's JSON, refreshed on document change or via the
 * "Re-evaluate" button.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MixerScreen(
    projectId: String?,
    signalGraph: SignalGraphKt? = null,
    errorMessage: String? = null,
    readLastEditError: () -> String? = { null },
    onSetMixer: (String, Double, Double, Boolean, Boolean) -> Unit = { _: String, _: Double, _: Double, _: Boolean, _: Boolean -> },
    onSetPreset: (String, String) -> Unit = { _: String, _: String -> },
    onEvaluateRouting: suspend () -> String = { "" },
    onNavigateBack: () -> Unit = {},
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

    var errorText by remember { mutableStateOf<String?>(null) }
    var routingReport by remember { mutableStateOf<String?>(null) }
    val scope = rememberCoroutineScope()

    val nodes = signalGraph?.nodes ?: emptyList()

    // Success refreshes the committed document — re-sync the inline error.
    LaunchedEffect(signalGraph) { errorText = readLastEditError() }

    // Failed mixer commits only set lastEditError (no state-flow emission), so
    // poll a short window after each commit to pick up the native message.
    fun pollLastError() {
        scope.launch {
            repeat(6) {
                delay(120)
                val e = readLastEditError()
                if (e != null) {
                    errorText = e
                    return@launch
                }
            }
            errorText = readLastEditError()
        }
    }

    // Re-evaluate the Routing desk whenever the document changes.
    suspend fun refreshRouting() {
        val raw = onEvaluateRouting()
        routingReport = if (raw.isBlank()) null else prettyJson(raw)
    }

    LaunchedEffect(signalGraph) { refreshRouting() }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(text = "Mixer") },
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
            // --- Channel strips ---------------------------------------------
            if (nodes.isEmpty()) {
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = "No mixer channels yet — add nodes in the Signal editor first",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            } else {
                LazyColumn(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    items(items = nodes, key = { it.id }) { node ->
                        mixerStrip(
                            node = node,
                            onSetMixer = onSetMixer,
                            onSetPreset = onSetPreset,
                            afterCommit = { pollLastError() },
                        )
                    }
                }
            }

            val shownError = errorText ?: errorMessage
            if (shownError != null) {
                Text(
                    text = shownError,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            // --- Routing preview card ---------------------------------------
            OutlinedCard(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(12.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            text = "Routing",
                            style = MaterialTheme.typography.titleSmall,
                            modifier = Modifier.weight(1f),
                        )
                        TextButton(
                            onClick = { scope.launch { refreshRouting() } },
                        ) { Text(text = "Re-evaluate") }
                    }
                    val report = routingReport
                    if (report == null) {
                        Text(
                            text = "No routing report (empty graph or cycle)",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    } else {
                        Text(
                            text = report,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            modifier = Modifier
                                .fillMaxWidth()
                                .heightIn(max = 160.dp)
                                .verticalScroll(rememberScrollState()),
                        )
                    }
                }
            }
        }
    }
}

/** One channel strip: name/kind header, gain (−60..24 dB) + pan (−1..1)
 *  sliders that commit on release, and mute/solo filter chips. Local slider
 *  state tracks the last committed document via the node fields. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun mixerStrip(
    node: SignalNodeKt,
    onSetMixer: (String, Double, Double, Boolean, Boolean) -> Unit,
    onSetPreset: (String, String) -> Unit,
    afterCommit: () -> Unit,
) {
    var gainDb by remember(node.id) { mutableStateOf(node.gainDb.toFloat()) }
    var pan by remember(node.id) { mutableStateOf(node.pan.toFloat()) }
    var mute by remember(node.id) { mutableStateOf(node.mute) }
    var solo by remember(node.id) { mutableStateOf(node.solo) }

    // Re-seed from the committed document (engine may clamp/round values).
    LaunchedEffect(node.gainDb, node.pan, node.mute, node.solo) {
        gainDb = node.gainDb.toFloat()
        pan = node.pan.toFloat()
        mute = node.mute
        solo = node.solo
    }

    fun commit() {
        onSetMixer(node.id, gainDb.toDouble(), pan.toDouble(), mute, solo)
        afterCommit()
    }

    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(text = node.name, style = MaterialTheme.typography.bodyLarge)
                    Text(
                        text = kindLabel(node.kind),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                val presetRef = node.dspPresetRef
                if (presetRef != null && presetRef.isNotBlank()) {
                    AssistChip(
                        // Re-apply the referenced preset to the engine.
                        onClick = {
                            onSetPreset(node.id, presetRef)
                            afterCommit()
                        },
                        label = { Text(text = "Preset: $presetRef") },
                    )
                }
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(text = "Gain", style = MaterialTheme.typography.labelMedium)
                Text(
                    text = "${gainDb.roundToInt()} dB",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.width(64.dp),
                )
                Slider(
                    value = gainDb,
                    onValueChange = { gainDb = it },
                    valueRange = -60f..24f,
                    onValueChangeFinished = { commit() },
                    modifier = Modifier.weight(1f),
                )
            }

            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(text = "Pan", style = MaterialTheme.typography.labelMedium)
                Text(
                    text = formatPan(pan),
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.width(64.dp),
                )
                Slider(
                    value = pan,
                    onValueChange = { pan = it },
                    valueRange = -1f..1f,
                    onValueChangeFinished = { commit() },
                    modifier = Modifier.weight(1f),
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilterChip(
                    selected = mute,
                    onClick = {
                        mute = !mute
                        commit()
                    },
                    label = { Text(text = "Mute") },
                )
                FilterChip(
                    selected = solo,
                    onClick = {
                        solo = !solo
                        commit()
                    },
                    label = { Text(text = "Solo") },
                )
            }
        }
    }
}