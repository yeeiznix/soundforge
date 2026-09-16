// SoundForge G2 — signal editor: node/edge add + remove over the shared
// signal-graph document (docs/PLAN_G2.md §4.6/P7). A pure view: node/edge
// lists arrive via UiState.Ready.signalGraph and every mutation goes through
// the SignalGraphViewModel callbacks wired here by NavGraph — no
// NativeBridge.* in this composable. Native failures land in
// ProjectViewModel.lastEditError WITHOUT a state-flow emission, so the screen
// polls lastEditError briefly after each commit; the committed-document
// refresh after a success re-syncs (and clears) the inline error.
package id.soundforge.pastudio.signal

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** Friendly display label for an SF_NODE_* kind. */
private fun kindLabel(kind: Int): String = when (kind) {
    SF_NODE_PROCESSOR -> "Processor"
    SF_NODE_OUTPUT -> "Output"
    SF_NODE_BUS -> "Bus"
    else -> "Source"
}

/** Resolve a node id to its display name (falls back to the raw id). */
private fun nodeNameOrId(nodes: List<SignalNodeKt>, id: String): String =
    nodes.firstOrNull { it.id == id }?.name ?: id

/**
 * G2 signal editor (PLAN_G2 §7.1). [projectId] null keeps the G0 "no project
 * open" affordance. Kind picker + name add nodes; from/to dropdowns + port
 * fields add edges; preset refs render as chips on node rows; remove buttons
 * delete node/edges. Errors surface inline: local validation text plus the
 * native message polled from [readLastEditError].
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SignalEditorScreen(
    projectId: String?,
    signalGraph: SignalGraphKt? = null,
    errorMessage: String? = null,
    readLastEditError: () -> String? = { null },
    onAddNode: (Int, String) -> Unit = { _: Int, _: String -> },
    onRemoveNode: (String) -> Unit = {},
    onAddEdge: (String, String, Int, Int) -> Unit = { _: String, _: String, _: Int, _: Int -> },
    onRemoveEdge: (String) -> Unit = {},
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

    var kind by remember { mutableStateOf(SF_NODE_SOURCE) }
    var nodeName by remember { mutableStateOf("") }
    var fromId by remember { mutableStateOf("") }
    var toId by remember { mutableStateOf("") }
    var fromPort by remember { mutableStateOf("") }
    var toPort by remember { mutableStateOf("") }
    var errorText by remember { mutableStateOf<String?>(null) }
    var fromExpanded by remember { mutableStateOf(false) }
    var toExpanded by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()

    val nodes = signalGraph?.nodes ?: emptyList()
    val edges = signalGraph?.edges ?: emptyList()

    // Success refreshes the committed document (new signalGraph instance) —
    // re-sync the inline error accordingly (lastEditError is cleared on
    // commit start and stays null on success).
    LaunchedEffect(signalGraph) { errorText = readLastEditError() }

    // Drop stale node selections after a document refresh (node removed).
    LaunchedEffect(nodes) {
        if (nodes.none { it.id == fromId }) fromId = ""
        if (nodes.none { it.id == toId }) toId = ""
    }

    // Failed graph commits only set lastEditError (no state-flow emission), so
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

    fun commitAddNode() {
        val name = nodeName.trim()
        if (name.isEmpty()) {
            errorText = "Enter a node name"
            return
        }
        errorText = null
        onAddNode(kind, name)
        nodeName = ""
        pollLastError()
    }

    fun commitAddEdge() {
        val fp = runCatching { fromPort.trim().toInt() }.getOrNull()
        val tp = runCatching { toPort.trim().toInt() }.getOrNull()
        when {
            fromId.isEmpty() || toId.isEmpty() ->
                errorText = "Pick both nodes for the connection"
            fromId == toId ->
                errorText = "A connection cannot link a node to itself"
            fp == null || tp == null ->
                errorText = "Ports must be whole numbers"
            else -> {
                errorText = null
                onAddEdge(fromId, toId, fp, tp)
                pollLastError()
            }
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(text = "Signal editor") },
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
            // --- Add node ---------------------------------------------------
            OutlinedCard(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(12.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(text = "Add node", style = MaterialTheme.typography.titleSmall)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        val kinds = listOf(
                            SF_NODE_SOURCE to "Source",
                            SF_NODE_PROCESSOR to "Processor",
                            SF_NODE_OUTPUT to "Output",
                            SF_NODE_BUS to "Bus",
                        )
                        kinds.forEach { (k, label) ->
                            FilterChip(
                                selected = kind == k,
                                onClick = { kind = k },
                                label = { Text(text = label) },
                            )
                        }
                    }
                    OutlinedTextField(
                        value = nodeName,
                        onValueChange = { nodeName = it },
                        singleLine = true,
                        label = { Text(text = "Node name") },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Button(
                        onClick = { commitAddNode() },
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(text = "Add node") }
                }
            }

            // --- Add edge ---------------------------------------------------
            OutlinedCard(modifier = Modifier.fillMaxWidth()) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(12.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Text(text = "Add connection", style = MaterialTheme.typography.titleSmall)
                    nodeDropdown(
                        label = "From node",
                        nodes = nodes,
                        selectedId = fromId,
                        expanded = fromExpanded,
                        onExpandedChange = { fromExpanded = it },
                        onSelect = { fromId = it },
                    )
                    nodeDropdown(
                        label = "To node",
                        nodes = nodes,
                        selectedId = toId,
                        expanded = toExpanded,
                        onExpandedChange = { toExpanded = it },
                        onSelect = { toId = it },
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedTextField(
                            value = fromPort,
                            onValueChange = { fromPort = it },
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                            label = { Text(text = "From port") },
                            modifier = Modifier.weight(1f),
                        )
                        OutlinedTextField(
                            value = toPort,
                            onValueChange = { toPort = it },
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                            label = { Text(text = "To port") },
                            modifier = Modifier.weight(1f),
                        )
                    }
                    Button(
                        onClick = { commitAddEdge() },
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text(text = "Add connection") }
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

            // --- Node + edge lists ------------------------------------------
            LazyColumn(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth(),
                contentPadding = PaddingValues(bottom = 16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                item(key = "nodesHeader") {
                    Text(
                        text = "Nodes — ${nodes.size}",
                        style = MaterialTheme.typography.titleSmall,
                    )
                }
                if (nodes.isEmpty()) {
                    item(key = "nodesEmpty") {
                        Text(
                            text = "No nodes yet — add one in the form above",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                } else {
                    items(items = nodes, key = { it.id }) { node ->
                        OutlinedCard(modifier = Modifier.fillMaxWidth()) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 12.dp, vertical = 8.dp),
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
                                        onClick = {},
                                        label = { Text(text = "Preset: $presetRef") },
                                    )
                                }
                                IconButton(
                                    onClick = {
                                        onRemoveNode(node.id)
                                        pollLastError()
                                    },
                                ) {
                                    Icon(
                                        imageVector = Icons.Default.Delete,
                                        contentDescription = "Remove ${node.name}",
                                    )
                                }
                            }
                        }
                    }
                }

                item(key = "edgesHeader") {
                    Text(
                        text = "Connections — ${edges.size}",
                        style = MaterialTheme.typography.titleSmall,
                        modifier = Modifier.padding(top = 8.dp),
                    )
                }
                if (edges.isEmpty()) {
                    item(key = "edgesEmpty") {
                        Text(
                            text = "No connections yet — link two nodes in the form above",
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                } else {
                    itemsIndexed(items = edges) { index, edge ->
                        OutlinedCard(modifier = Modifier.fillMaxWidth()) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 12.dp, vertical = 8.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                Column(modifier = Modifier.weight(1f)) {
                                    Text(
                                        text = "${nodeNameOrId(nodes, edge.fromNodeId)} → " +
                                            nodeNameOrId(nodes, edge.toNodeId),
                                        style = MaterialTheme.typography.bodyLarge,
                                    )
                                    if (edge.fromPort != 0 || edge.toPort != 0) {
                                        Text(
                                            text = "ports ${edge.fromPort} → ${edge.toPort}",
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        )
                                    }
                                }
                                IconButton(
                                    onClick = {
                                        // Edge ids cross the v2 wire as of P7
                                        // (additive schema refresh), so removeEdge
                                        // targets the real edge id.
                                        onRemoveEdge(edge.id)
                                        pollLastError()
                                    },
                                ) {
                                    Icon(
                                        imageVector = Icons.Default.Delete,
                                        contentDescription = "Remove connection ${index + 1}",
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/** Per-spec from/to node picker (same ExposedDropdownMenuBox pattern as the
 *  G1 SceneEditorScreen). Empty node list still opens, showing a hint entry. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun nodeDropdown(
    label: String,
    nodes: List<SignalNodeKt>,
    selectedId: String,
    expanded: Boolean,
    onExpandedChange: (Boolean) -> Unit,
    onSelect: (String) -> Unit,
) {
    ExposedDropdownMenuBox(
        expanded = expanded,
        onExpandedChange = onExpandedChange,
    ) {
        OutlinedTextField(
            value = if (selectedId.isEmpty()) "None" else nodeNameOrId(nodes, selectedId),
            onValueChange = {},
            readOnly = true,
            label = { Text(text = label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
            modifier = Modifier
                .menuAnchor()
                .fillMaxWidth(),
        )
        ExposedDropdownMenu(
            expanded = expanded,
            onDismissRequest = { onExpandedChange(false) },
        ) {
            if (nodes.isEmpty()) {
                DropdownMenuItem(
                    text = { Text(text = "No nodes yet — add one above") },
                    onClick = { onExpandedChange(false) },
                )
            } else {
                nodes.forEach { node ->
                    DropdownMenuItem(
                        text = { Text(text = node.name) },
                        onClick = {
                            onExpandedChange(false)
                            onSelect(node.id)
                        },
                    )
                }
            }
        }
    }
}