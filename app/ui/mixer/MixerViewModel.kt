// SoundForge G2 — ViewModel for the mixer screen (PLAN_G2 §4.6/P6). Same
// g1.1-hardened shape as SignalGraphViewModel: the PROJECT handle and its
// nativeMutex are shared with ProjectViewModel; every commit runs under that
// mutex; succeeded edits refresh via ProjectViewModel.refreshState().
package id.soundforge.pastudio.mixer

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

class MixerViewModel(application: Application) : AndroidViewModel(application) {

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

    /** Run a mixer edit under the shared mutex; refresh on success. */
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

    fun setMixer(nodeId: String, gainDb: Double, pan: Double, mute: Boolean, solo: Boolean) =
        commit { h ->
            NativeBridge.graphSetMixer(h, nodeId, gainDb, pan, mute, solo) == SF_OK
        }

    fun setPreset(nodeId: String, presetId: String) = commit { h ->
        NativeBridge.graphSetPreset(h, nodeId, presetId) == SF_OK
    }

    /** Static routing-desk evaluation JSON ("Routing" preview card, P7); "" on
     *  error (cycle etc. — read lastEditError). */
    suspend fun evaluateMixerReport(): String = project?.let { owner ->
        owner.nativeMutex.withLock {
            if (handle == 0L) "" else NativeBridge.graphEvaluateMixer(handle)
        }
    } ?: ""

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