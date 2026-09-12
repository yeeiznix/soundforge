// SoundForge G0 — default uncaught exception handler; persists crash context.
package id.soundforge.pastudio.platform.diagnostics

import android.content.Context
import android.util.Log
import id.soundforge.pastudio.platform.bridge.NativeBridge
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Installs a default uncaught exception handler that appends a timestamped
 * stack trace to filesDir/crash.log, flushes the native ring buffer, and then
 * delegates to the previous handler so the platform crash flow is preserved.
 */
object CrashHandler {

    fun install(context: Context) {
        val appContext = context.applicationContext
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            runCatching {
                val stamp = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply {
                    timeZone = TimeZone.getTimeZone("UTC")
                }.format(Date())
                File(appContext.filesDir, "crash.log").appendText(
                    stamp + " thread=" + thread.name + "\n" + Log.getStackTraceString(throwable) + "\n\n",
                )
                NativeBridge.flushLogs(SfLogger.logPath(appContext))
            }
            // Always chain to the platform handler (crash dialog / process death).
            previous?.uncaughtException(thread, throwable)
        }
    }
}
