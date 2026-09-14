// SoundForge G0 — Kotlin JNI facade over the sfcore C ABI (plan §4.3).
// Thin by design: one `external fun` per C ABI entry, no logic here.
// libsfcore.so is produced by app/src/main/cpp/CMakeLists.txt (jni_bridge.cpp
// plus the sfcore core sources compiled into a single shared library).
//
// JNI_OnLoad (native side) installs a logcat log sink via sf_set_log_sink, so
// every sf_log line appears under "SF/<tag>" with no Java-side callback.
// 26 external funs below (17 G0/G1/G3 + 9 G2 graph) — their symbols use the full
// JNI prefix Java_id_soundforge_pastudio_platform_bridge_NativeBridge_<name>.
//
// G0: synchronous calls; G2 introduces SfCommandQueue for audio-thread safety.
package id.soundforge.pastudio.platform.bridge

object NativeBridge {
    init {
        System.loadLibrary("sfcore")
    }

    // --- Version -------------------------------------------------------------
    external fun engineVersion(): String
    external fun schemaVersion(): Int
    external fun isCompatible(schemaVersion: Int): Boolean

    // --- Editor mutators (G1) ------------------------------------------------
    // Field-level project edits (PLAN_G1 §4.2). Return the SF_* result code;
    // map `!= SF_OK` to lastError(handle) read on the same thread.
    external fun renameProject(handle: Long, newName: String): Int
    external fun renameVenue(handle: Long, newName: String): Int
    // G3 scene-name edit (docs/PLAN_G3.md §4.5): renames the scene object, NOT
    // the project — sf_scene_rename reuses the scene.update audit action.
    external fun sceneRename(handle: Long, newName: String): Int
    external fun setVenueDimensions(handle: Long, w: Double, d: Double, h: Double): Int
    external fun setSceneGeometry(handle: Long, cx: Double, cy: Double, cz: Double,
                                  lx: Double, ly: Double, lz: Double): Int

    // --- Graph editor (G2) ---------------------------------------------------
    // Signal-graph edits (PLAN_G2 §4.6), passthroughs to the sf_graph_* C ABI.
    // Node/edge ids return as strings ("" on error — read lastError(handle));
    // mutators return SF_* codes like the G1 editors.
    external fun graphAddNode(handle: Long, kind: Int, name: String): String // new node UUID, "" on error
    external fun graphRemoveNode(handle: Long, nodeId: String): Int
    external fun graphAddEdge(handle: Long, fromId: String, toId: String,
                              fromPort: Int, toPort: Int): String // new edge UUID, "" on error
    external fun graphRemoveEdge(handle: Long, edgeId: String): Int
    external fun graphSetMixer(handle: Long, nodeId: String, gainDb: Double, pan: Double,
                               mute: Boolean, solo: Boolean): Int
    external fun graphSetPreset(handle: Long, nodeId: String, presetId: String): Int // "" clears

    // Structural routing reports as JSON strings; "" on error (lastError).
    // graphTopologicalOrder / graphEvaluateMixer fail with SF_E_SCHEMA on
    // cycles — read lastError(handle) for the reason.
    external fun graphValidate(handle: Long): String
    external fun graphTopologicalOrder(handle: Long): String
    external fun graphEvaluateMixer(handle: Long): String

    // --- Lifecycle -----------------------------------------------------------
    // Handles are opaque jlongs; 0L means "no project". On failure the core
    // records a human-readable message; lastError(0L) reads the CALLING
    // thread's thread-local error, so check it on the same thread.
    external fun projectCreate(name: String, author: String?): Long
    external fun projectDestroy(handle: Long)

    // --- JSON codec ----------------------------------------------------------
    // G0: projectToJson returns "" on failure instead of throwing; treat an
    // empty string as an error and read lastError(handle) for the reason.
    external fun projectToJson(handle: Long): String
    external fun projectFromJson(json: String): Long // 0L on error

    // --- Validation / health -------------------------------------------------
    external fun validateJson(json: String): String? // null = valid, else error text

    // JSON report: {"status":"ok"|"warning"|"error","warnings":[],"errors":[],"stats":{}}
    external fun healthCheck(handle: Long): String

    // --- Diagnostics ---------------------------------------------------------
    external fun lastError(handle: Long): String // handle 0L = thread-local error
    external fun log(level: Int, tag: String, msg: String) // level: SF_LOG_* (0..3)

    // Appends the crash-safe ring buffer to `path` and drains it; returns the
    // SF_* result code (SfLogger passes filesDir/logs/sf.log).
    external fun flushLogs(path: String): Int
}
