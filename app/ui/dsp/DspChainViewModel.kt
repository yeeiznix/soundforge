// SoundForge G3 — ViewModel for the DSP chain screen (PLAN_G3 §6 P6).
// Same g1.1-hardened shape as SignalGraphViewModel / MixerViewModel: the
// PROJECT handle and its nativeMutex are shared with ProjectViewModel; every
// commit runs under that mutex; succeeded edits refresh via
// ProjectViewModel.refreshState(). The VM surface is one mutator —
// setPreset(nodeId, presetId) — which maps to sf_graph_set_preset
// ("" = clear), already exported through NativeBridge since G2.
package id.soundforge.pastudio.dsp

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import id.soundforge.pastudio.platform.bridge.NativeBridge
import id.soundforge.pastudio.project.ProjectViewModel
import id.soundforge.pastudio.project.UiState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.sync.withLock

/** SF_OK (sf_types.h); bridge graph mutators return SF_* codes. */
private val SF_OK = 0

class DspChainViewModel(application: Application) : AndroidViewModel(application) {

    /** Owner of the shared project handle + mutex; null until [bind]. */
    private var project: ProjectViewModel? = null

    /** Live project handle mirrored from ProjectViewModel.state; 0L = none. */
    @Volatile
    var handle: Long = 0L
        private set

    private var handleJob: Job? = null

    /** Bind to the shared project document (called by NavGraph once Ready). */
    fun bind(owner: ProjectViewModel) {
        if (project === owner) return
        project = owner
        handle = (owner.state.value as? UiState.Ready)?.handle ?: 0L
        handleJob?.cancel()
        handleJob = viewModelScope.launch(Dispatchers.Main) {
            owner.state.collect { s -> handle = (s as? UiState.Ready)?.handle ?: 0L }
        }
    }

    /** Run a DSP-chain edit under the shared mutex; refresh on success. */
    private fun commit(edit: suspend (handle: Long) -> Boolean) {
        val owner = project ?: return
        viewModelScope.launch(Dispatchers.IO) {
            owner.nativeMutex.withLock {
                if (handle == 0L) {
                    owner.lastEditError = "No project is open"
                    return@launch
                }
                owner.lastEditError = null
                if (!edit(handle)) {
                    owner.lastEditError = NativeBridge.lastError(handle)
                    return@launch
                }
            }
            owner.refreshState()  // outside the lock (Mutex is not reentrant)
        }
    }

    /** Attach / replace / clear a preset on a signal node. "" clears. */
    fun setPreset(nodeId: String, presetId: String) = commit { h ->
        NativeBridge.graphSetPreset(h, nodeId, presetId) == SF_OK
    }

    /** Join in-flight commits; the handle itself is released by
     *  ProjectViewModel.close() (single owner). */
    fun close() {
        handleJob?.cancel()
        project?.let { owner -> runBlocking { owner.nativeMutex.withLock {} } }
    }

    override fun onCleared() {
        close()
        super.onCleared()
    }
}