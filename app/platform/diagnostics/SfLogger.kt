// SoundForge G0 — crash-safe diagnostics: log flushing + crash persistence.
package id.soundforge.pastudio.platform.diagnostics

import android.content.Context
import android.os.Handler
import android.os.Looper
import id.soundforge.pastudio.platform.bridge.NativeBridge
import java.io.File

/**
 * Bridges Kotlin-side logging into the native crash-safe ring buffer and
 * periodically drains it to filesDir/logs/sf.log.
 */
object SfLogger {

    /** Latest audio capability probe report, surfaced on the home screen. */
    var lastProbeReport: String? = null

    private val handler = Handler(Looper.getMainLooper())
    private var appContext: Context? = null
    private var started = false

    private val flushLoop = object : Runnable {
        override fun run() {
            appContext?.let { context ->
                // runCatching: host instrumentation runs ship without libsfcore.so,
                // and a failed flush must never take the app down.
                runCatching { NativeBridge.flushLogs(logPath(context)) }
            }
            handler.postDelayed(this, FLUSH_INTERVAL_MS)
        }
    }

    /** Starts the periodic native log flush (every 5 s). Idempotent. */
    fun init(context: Context) {
        if (started) return
        started = true
        appContext = context.applicationContext
        handler.postDelayed(flushLoop, FLUSH_INTERVAL_MS)
    }

    /** Absolute path of the crash-safe log file; creates the logs directory. */
    fun logPath(context: Context): String {
        val file = File(File(context.filesDir, "logs"), "sf.log")
        file.parentFile?.mkdirs()
        return file.toString()
    }

    /** Forward a line to the native ring buffer (level 0..3; 1 = INFO). */
    fun log(level: Int, tag: String, msg: String) = NativeBridge.log(level, tag, msg)

    private const val FLUSH_INTERVAL_MS = 5_000L
}
