// SoundForge G2 — ViewModel for the signal editor screen (PLAN_G2 §4.6/P6).
// g1.1-hardened shape: the PROJECT handle and its nativeMutex are shared with
// ProjectViewModel (sf_project_* is not thread-safe per handle, so the lock is
// per-handle, not per-VM); every graph commit runs under that mutex; succeeded
// edits re-parse the committed document through ProjectViewModel.refreshState()
// — committed-doc-is-truth, same as the G1 editors.
package id.soundforge.pastudio.signal

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

class SignalGraphViewModel(application: Application) : AndroidViewModel(application) {

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

    /** Run a graph edit under the shared mutex; refresh state from the
     *  committed document on success, surface the native error otherwise. */
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

    fun addNode(kind: Int, name: String) = commit { h ->
        NativeBridge.graphAddNode(h, kind, name).isNotEmpty()
    }

    fun removeNode(nodeId: String) = commit { h ->
        NativeBridge.graphRemoveNode(h, nodeId) == SF_OK
    }

    fun addEdge(fromId: String, toId: String, fromPort: Int, toPort: Int) = commit { h ->
        NativeBridge.graphAddEdge(h, fromId, toId, fromPort, toPort).isNotEmpty()
    }

    fun removeEdge(edgeId: String) = commit { h ->
        NativeBridge.graphRemoveEdge(h, edgeId) == SF_OK
    }

    /** Structural report ({status,warnings,errors,stats}); "" on error. */
    suspend fun validateReport(): String = project?.let { owner ->
        owner.nativeMutex.withLock {
            if (handle == 0L) "" else NativeBridge.graphValidate(handle)
        }
    } ?: ""

    /** Topological order JSON ({"order":[...]}); "" on cycle/error. */
    suspend fun topologicalOrderReport(): String = project?.let { owner ->
        owner.nativeMutex.withLock {
            if (handle == 0L) "" else NativeBridge.graphTopologicalOrder(handle)
        }
    } ?: ""

    /** Join in-flight commits; the handle itself is released by
     *  ProjectViewModel.close() (single owner) — this VM only waits out the
     *  shared mutex at teardown so no commit is in flight during destroy. */
    fun close() {
        handleJob?.cancel()
        project?.let { owner -> runBlocking { owner.nativeMutex.withLock {} } }
    }

    override fun onCleared() {
        close()
        super.onCleared()
    }
}