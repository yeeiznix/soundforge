// SoundForge G0 — UI-side project state holder over NativeBridge handles.
// One shared instance is hoisted in SoundForgeNavGraph and forwarded to the
// home + project list screens; all native work runs on Dispatchers.IO.
package id.soundforge.pastudio.project

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import id.soundforge.pastudio.platform.bridge.NativeBridge
import id.soundforge.pastudio.platform.storage.FileStorageRepository
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import org.json.JSONObject

/** UI-facing state of the currently open project. */
sealed interface UiState {
    /** Initial state: no create/open attempt has completed yet. */
    object Loading : UiState

    /** A native project handle is alive; [meta] mirrors the open document. */
    data class Ready(val meta: ProjectMetaKt, val healthReport: String?) : UiState

    /** The last create/open/save attempt failed; no usable handle. */
    data class Error(val message: String) : UiState
}

/** Snapshot of project metadata parsed from NativeBridge.projectToJson JSON. */
data class ProjectMetaKt(
    val id: Long,
    val name: String,
    val createdAt: String,
    val modifiedAt: String,
    val schemaVersion: Int,
    val engineVersion: String,
)

class ProjectViewModel(application: Application) : AndroidViewModel(application) {

    private val repo = FileStorageRepository(File(application.filesDir))

    /** Opaque native project handle; 0L = none. Mutated only inside coroutines. */
    var handle: Long = 0L
        private set

    private val _state = MutableStateFlow<UiState>(UiState.Loading)

    /** Observable UI state. */
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** Create a new native project and adopt its handle. */
    fun create(name: String) {
        viewModelScope.launch(Dispatchers.IO) {
            _state.value = UiState.Loading
            val newHandle = NativeBridge.projectCreate(name, null)
            if (newHandle == 0L) {
                _state.value = UiState.Error(NativeBridge.lastError(0L))
                return@launch
            }
            handle = newHandle
            _state.value = readyState(newHandle, fallbackName = name)
        }
    }

    /** Open a project document: repo read + NativeBridge.projectFromJson. */
    fun openFromPath(path: String) {
        viewModelScope.launch(Dispatchers.IO) {
            _state.value = UiState.Loading
            val bytes = repo.readProject(path).getOrElse { error ->
                _state.value = UiState.Error(error.message ?: "Unable to read project")
                return@launch
            }
            val opened = NativeBridge.projectFromJson(String(bytes, Charsets.UTF_8))
            if (opened == 0L) {
                _state.value = UiState.Error(NativeBridge.lastError(0L))
                return@launch
            }
            handle = opened
            _state.value = readyState(opened, fallbackName = File(path).nameWithoutExtension)
        }
    }

    /** Serialize the open project and persist it at [path] via the repo. */
    fun saveTo(path: String) {
        viewModelScope.launch(Dispatchers.IO) {
            if (handle == 0L) {
                _state.value = UiState.Error("No project is open")
                return@launch
            }
            val previous = _state.value as? UiState.Ready
            val json = NativeBridge.projectToJson(handle)
            if (json.isEmpty()) {
                _state.value = UiState.Error(NativeBridge.lastError(handle))
                return@launch
            }
            repo.writeProject(path, json.toByteArray(Charsets.UTF_8))
                .onSuccess {
                    _state.value = UiState.Ready(
                        // Keep parsed metadata; only the modification stamp moves.
                        meta = previous?.meta?.copy(modifiedAt = isoNow())
                            ?: parseMeta(json, File(path).nameWithoutExtension),
                        healthReport = previous?.healthReport,
                    )
                }
                .onFailure { error ->
                    _state.value = UiState.Error(error.message ?: "Unable to write project")
                }
        }
    }

    /** Release the native handle; also invoked automatically from [onCleared]. */
    fun close() {
        if (handle != 0L) {
            runCatching { NativeBridge.projectDestroy(handle) }
            handle = 0L
            _state.value = UiState.Loading
        }
    }

    override fun onCleared() {
        close()
        super.onCleared()
    }

    /** Build Ready from a live handle, attaching a best-effort health report. */
    private fun readyState(nativeHandle: Long, fallbackName: String): UiState.Ready {
        val json = NativeBridge.projectToJson(nativeHandle)
        val meta = if (json.isEmpty()) {
            ProjectMetaKt(
                id = 0L,
                name = fallbackName,
                createdAt = isoNow(),
                modifiedAt = "",
                schemaVersion = NativeBridge.schemaVersion(),
                engineVersion = NativeBridge.engineVersion(),
            )
        } else {
            parseMeta(json, fallbackName)
        }
        val health = runCatching { NativeBridge.healthCheck(nativeHandle) }
            .getOrNull()
            ?.takeIf { it.isNotBlank() }
        return UiState.Ready(meta, health)
    }

    /** Tolerant org.json parse of the projectToJson document. */
    private fun parseMeta(json: String, fallbackName: String): ProjectMetaKt = runCatching {
        val obj = JSONObject(json)
        ProjectMetaKt(
            id = obj.optLong("id", 0L),
            name = obj.optString("name", fallbackName),
            createdAt = obj.optString("createdAt", ""),
            modifiedAt = obj.optString("modifiedAt", ""),
            schemaVersion = obj.optInt("schemaVersion", 0),
            engineVersion = obj.optString("engineVersion", ""),
        )
    }.getOrElse {
        // Malformed/empty JSON: fall back to a minimal, still-renderable meta.
        ProjectMetaKt(id = 0L, name = fallbackName, createdAt = "", modifiedAt = "", schemaVersion = 0, engineVersion = "")
    }

    private fun isoNow(): String = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply {
        timeZone = TimeZone.getTimeZone("UTC")
    }.format(Date())
}
