// SoundForge G0 — application entry: diagnostics bootstrap + capability probe.
package id.soundforge.pastudio

import android.app.Application
import id.soundforge.pastudio.platform.audio.DefaultAudioCapabilityProbe
import id.soundforge.pastudio.platform.diagnostics.CrashHandler
import id.soundforge.pastudio.platform.diagnostics.SfLogger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

class SoundForgeApplication : Application() {

    /** Fire-and-forget scope for startup IO; children never cancel the scope. */
    private val appScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    override fun onCreate() {
        super.onCreate()
        SfLogger.init(this)
        CrashHandler.install(this)
        appScope.launch {
            // runCatching: host instrumentation runs ship without libsfcore.so;
            // a failed probe must not crash the process at startup.
            runCatching {
                val probe = DefaultAudioCapabilityProbe()
                val report = probe.probe()
                val formatted = probe.formatForLog(report)
                SfLogger.lastProbeReport = formatted
                SfLogger.log(1, "probe", formatted) // level 1 = INFO
            }
        }
    }
}
