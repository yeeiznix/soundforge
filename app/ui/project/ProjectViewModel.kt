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
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import id.soundforge.pastudio.scene.Pt3
import id.soundforge.pastudio.scene.SceneKt
import id.soundforge.pastudio.scene.projectSceneOf
import id.soundforge.pastudio.dsp.DspPresetKt
import id.soundforge.pastudio.dsp.projectDspPresetsOf
import id.soundforge.pastudio.signal.SignalGraphKt
import id.soundforge.pastudio.signal.projectSignalGraphOf
import id.soundforge.pastudio.venue.VenueKt
import id.soundforge.pastudio.venue.projectVenueOf
import org.json.JSONObject

/** UI-facing state of the currently open project. */
sealed interface UiState {
    /** Initial state: no create/open attempt has completed yet. */
    object Loading : UiState

    /** A native project handle is alive; [meta] mirrors the open document. */
    data class Ready(
        val meta: ProjectMetaKt,
        val healthReport: String?,
        val handle: Long = 0L,
        val scene: SceneKt? = null,
        val venue: VenueKt? = null,
        val signalGraph: SignalGraphKt? = null,
        /** Existing dspPresets envelopes (attach/replace/clear targets, §6 P6). */
        val dspPresets: List<DspPresetKt> = emptyList(),
    ) : UiState

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

/** SF_OK (sf_types.h). The bridge mutators return SF_* codes. */
private val SF_OK = 0

/**
 * Built-in venue presets -> default room dimensions (meters), PLAN_G1 §6.4.
 * "Small Club" mirrors the engine defaults (SF_ROOM_DEFAULT_*); the larger
 * presets pick representative rooms. Keys must match NewProjectDialog.
 */
val BuiltInVenuePresetDims: Map<String, Triple<Double, Double, Double>> = mapOf(
    "Small Club" to Triple(12.0, 10.0, 4.0),
    "Warehouse" to Triple(24.0, 18.0, 6.0),
    "Theater" to Triple(30.0, 20.0, 10.0),
)

class ProjectViewModel(application: Application) : AndroidViewModel(application) {

    private val repo = FileStorageRepository(File(application.filesDir))

    /**
     * Serializes all native-handle-touching work; close() joins via the same
     * mutex (sf_project_* is not thread-safe per handle). Shared with the G2
     * graph ViewModels (PLAN_G2 §4.6): ONE mutex per handle, not per VM.
     */
    internal val nativeMutex = Mutex()

    /** Opaque native project handle; 0L = none. Mutated only inside coroutines. */
    @Volatile var handle: Long = 0L
        private set

    /** Last native error from an edit helper (read after a failed commit). */
    @Volatile var lastEditError: String? = null

    private val _state = MutableStateFlow<UiState>(UiState.Loading)

    /** Observable UI state. */
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** Create a new native project and adopt its handle. */
    fun create(name: String, venuePreset: String = "") {
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
                _state.value = UiState.Loading
                val newHandle = NativeBridge.projectCreate(name, null)
                if (newHandle == 0L) {
                    _state.value = UiState.Error(NativeBridge.lastError(0L))
                    return@launch
                }
                handle = newHandle
                persistVenuePreset(newHandle, venuePreset)
                _state.value = readyState(newHandle, fallbackName = name)
            }
        }
    }

    /**
     * Writes ONLY the venue dimensions from the creation-time preset
     * (PLAN_G1 §6.4): named presets drive the room dimensions, free-form
     * names keep the engine defaults. The project rename is deliberately
     * skipped — projectCreate(name, null) already named the new document, so
     * renaming again here would emit a duplicate `project.create` +
     * `project.rename` audit pair on every new project.
     */
    private fun persistVenuePreset(newHandle: Long, venuePreset: String) {
        val dims = BuiltInVenuePresetDims[venuePreset]
        if (dims != null) {
            NativeBridge.setVenueDimensions(newHandle, dims.first, dims.second, dims.third)
        }
    }

    /** Field-level scene geometry edit (PLAN_G1 §4.2/§6.4). Failed edits keep
     *  the handle valid; the native error message is exposed via [lastEditError]
     *  (and the UI state rolls back to the last committed document). */
    fun updateSceneGeometry(center: Pt3, listening: Pt3) {
        lastEditError = null
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
                if (handle == 0L) {
                    lastEditError = "No project is open"
                    return@launch
                }
                if (NativeBridge.setSceneGeometry(
                        handle, center.x, center.y, center.z,
                        listening.x, listening.y, listening.z,
                    ) != SF_OK
                ) {
                    lastEditError = NativeBridge.lastError(handle)
                    _state.value = UiState.Error(lastEditError ?: "Scene update failed")
                    return@launch
                }
                _state.value = readyState(handle, fallbackName = currentName())
            }
        }
    }

    /** Field-level venue edit (name or dimensions). See [updateSceneGeometry]. */
    fun updateVenue(name: String, widthM: Double, depthM: Double, heightM: Double) {
        lastEditError = null
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
                if (handle == 0L) {
                    lastEditError = "No project is open"
                    return@launch
                }
                if (NativeBridge.renameVenue(handle, name) != SF_OK ||
                    NativeBridge.setVenueDimensions(handle, widthM, depthM, heightM) != SF_OK
                ) {
                    lastEditError = NativeBridge.lastError(handle)
                    _state.value = UiState.Error(lastEditError ?: "Venue update failed")
                    return@launch
                }
                _state.value = readyState(handle, fallbackName = name)
            }
        }
    }

    /** Field-level project rename (plan §6.2 scene name commit). */
    fun renameProject(name: String) {
        lastEditError = null
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
                if (handle == 0L) {
                    lastEditError = "No project is open"
                    return@launch
                }
                if (NativeBridge.renameProject(handle, name) != SF_OK) {
                    lastEditError = NativeBridge.lastError(handle)
                    _state.value = UiState.Error(lastEditError ?: "Rename failed")
                    return@launch
                }
                _state.value = readyState(handle, fallbackName = name)
            }
        }
    }

    /** Field-level scene rename (PLAN_G3 §4.5/§6 P6): renames doc.scene.name
     *  only — project.name is untouched (freeze invariant). Mirrors
     *  [renameProject]: failure keeps the handle valid and surfaces the native
     *  error via [lastEditError] + a UiState.Error rollback. */
    fun sceneRename(name: String) {
        lastEditError = null
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
                if (handle == 0L) {
                    lastEditError = "No project is open"
                    return@launch
                }
                if (NativeBridge.sceneRename(handle, name) != SF_OK) {
                    lastEditError = NativeBridge.lastError(handle)
                    _state.value = UiState.Error(lastEditError ?: "Scene rename failed")
                    return@launch
                }
                _state.value = readyState(handle, fallbackName = currentName())
            }
        }
    }

    /** Re-parse the committed document into [UiState.Ready] without touching
     *  the handle (committed-doc-is-truth). Called by the G2 graph ViewModels
     *  after a successful sf_graph_* commit — call OUTSIDE a nativeMutex
     *  withLock block (it takes the lock itself; Mutex is not reentrant). */
    internal fun refreshState() {
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
                if (handle != 0L) _state.value = readyState(handle, currentName())
            }
        }
    }

    private fun currentName(): String =
        (_state.value as? UiState.Ready)?.meta?.name ?: "Untitled"

    /** Open a project document: repo read + NativeBridge.projectFromJson. */
    fun openFromPath(path: String) {
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
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
    }

    /** Serialize the open project and persist it at [path] via the repo. */
    fun saveTo(path: String) {
        viewModelScope.launch(Dispatchers.IO) {
            nativeMutex.withLock {
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
                            handle = previous?.handle ?: 0L,
                            scene = previous?.scene,
                            venue = previous?.venue,
                            signalGraph = previous?.signalGraph,
                            dspPresets = previous?.dspPresets ?: emptyList(),
                        )
                    }
                    .onFailure { error ->
                        _state.value = UiState.Error(error.message ?: "Unable to write project")
                    }
            }
        }
    }

    /** Release the native handle; also invoked automatically from [onCleared]. */
    fun close() {
        runBlocking {
            nativeMutex.withLock {
                if (handle != 0L) {
                    runCatching { NativeBridge.projectDestroy(handle) }
                    handle = 0L
                    _state.value = UiState.Loading
                }
            }
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
        val root = if (json.isEmpty()) null else runCatching { JSONObject(json) }.getOrNull()
        return UiState.Ready(
            meta = meta,
            healthReport = health,
            handle = nativeHandle,
            scene = root?.let { projectSceneOf(it) },
            venue = root?.let { projectVenueOf(it) },
            signalGraph = root?.let { projectSignalGraphOf(it) },
            dspPresets = root?.let { projectDspPresetsOf(it) } ?: emptyList(),
        )
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
