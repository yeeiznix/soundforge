// SoundForge G3 — DSP chain editor: per-node preset attach / replace / clear
// over the project's existing dspPresets envelopes (docs/PLAN_G3.md §6 P6,
// D7 dirty flags). A pure view: nodes + envelope list arrive via
// UiState.Ready (signalGraph/dspPresets) and every commit goes through the
// DspChainViewModel callbacks wired here by NavGraph — no NativeBridge.* in
// this composable. As in SignalEditorScreen/MixerScreen, native failures only
// set ProjectViewModel.lastEditError (no state-flow emission), so commits poll
// it briefly; the committed-document refresh after a success re-syncs the
// inline error. Each node row stages its preset choice in a DirtyField: chips
// set the pending selection, "Apply" (enabled only when dirty) commits, and
// the refresh re-seeds — never clobbering a pending edit.
package id.soundforge.pastudio.dsp

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Scaffold
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
import androidx.compose.ui.unit.dp
import id.soundforge.pastudio.common.DirtyField
import id.soundforge.pastudio.signal.SF_NODE_BUS
import id.soundforge.pastudio.signal.SF_NODE_OUTPUT
import id.soundforge.pastudio.signal.SF_NODE_PROCESSOR
import id.soundforge.pastudio.signal.SignalGraphKt
import id.soundforge.pastudio.signal.SignalNodeKt
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** Friendly display label for an SF_NODE_* kind (mirrored from other screens). */
private fun kindLabel(kind: Int): String = when (kind) {
    SF_NODE_PROCESSOR -> "Processor"
    SF_NODE_OUTPUT -> "Output"
    SF_NODE_BUS -> "Bus"
    else -> "Source"
}

/**
 * G3 DSP chain editor (PLAN_G3 §6). [projectId] null keeps the G0 "no project
 * open" affordance. Each signal node shows its current preset attachment and
 * lets the user pick a different preset envelope; a two-step process —
 * tap chips to stage (DirtyField.edit), then press Apply (commits via
 * [onSetPreset]). Errors surface inline through [readLastEditError] polling.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DspChainScreen(
    projectId: String?,
    signalGraph: SignalGraphKt? = null,
    dspPresets: List<DspPresetKt>? = null,
    errorMessage: String? = null,
    readLastEditError: () -> String? = { null },
    onSetPreset: (String, String) -> Unit = { _: String, _: String -> },
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
    val scope = rememberCoroutineScope()

    val nodes = signalGraph?.nodes ?: emptyList()
    val presets = dspPresets ?: emptyList()

    // Success refreshes the committed document — re-sync the inline error.
    LaunchedEffect(signalGraph) { errorText = readLastEditError() }

    // Failed commits only set lastEditError (no state-flow emission), so
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

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(text = "DSP chain") },
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
            Text(
                text = "Pick a preset for a node, then press Apply. Clear removes it.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            if (presets.isEmpty()) {
                Text(
                    text = "No DSP presets in this project — only the Clear action is available.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            val shownError = errorText ?: errorMessage
            if (shownError != null) {
                Text(
                    text = shownError,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            if (nodes.isEmpty()) {
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        text = "No nodes yet — add them in the Signal editor first",
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
                        presetNodeCard(
                            node = node,
                            presets = presets,
                            onApply = { nodeId, presetId ->
                                onSetPreset(nodeId, presetId)
                                pollLastError()
                            },
                        )
                    }
                }
            }
        }
    }
}

/** One node card: header (name + kind), preset-chip row, and Apply button. */
@Composable
private fun presetNodeCard(
    node: SignalNodeKt,
    presets: List<DspPresetKt>,
    onApply: (String, String) -> Unit,
) {
    val selection = remember(node.id) { DirtyField(node.dspPresetRef ?: "") }

    // Re-seed on document refresh — never clobbers a pending edit.
    LaunchedEffect(node.dspPresetRef) { selection.seed(node.dspPresetRef ?: "") }

    // Currently displayed selection: the pending value when dirty, else committed ref.
    val current = if (selection.isDirty) selection.value else (node.dspPresetRef ?: "")

    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // --- Node header --------------------------------------------------
            Column {
                Text(text = node.name, style = MaterialTheme.typography.bodyLarge)
                Text(
                    text = kindLabel(node.kind),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            // --- Preset chips + Clear ----------------------------------------
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.horizontalScroll(rememberScrollState()),
            ) {
                presets.forEach { preset ->
                    FilterChip(
                        selected = current == preset.id,
                        onClick = { selection.edit(preset.id) },
                        label = { Text(text = preset.displayName) },
                    )
                }
                FilterChip(
                    selected = current.isEmpty(),
                    onClick = { selection.edit("") },
                    label = { Text(text = "Clear") },
                )
            }

            // --- Apply (gated on dirty) --------------------------------------
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.End,
            ) {
                TextButton(
                    onClick = {
                        onApply(node.id, selection.value)
                        selection.commit()
                    },
                    enabled = selection.isDirty,
                ) { Text(text = "Apply") }
            }
        }
    }
}
