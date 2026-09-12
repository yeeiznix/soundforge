// SoundForge G0 — audio environment probe (hardcoded conservative defaults).
package id.soundforge.pastudio.platform.audio

import android.os.Build
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

/**
 * Reports the device's audio capabilities for the home-screen diagnostics
 * chip. A future implementation queries AAudio/AudioManager at runtime.
 */
interface AudioCapabilityProbe {

    /** Point-in-time snapshot of the audio environment. */
    data class Report(
        val osVersion: String,
        val abi: String,
        val lowLatencySupported: Boolean,
        val sampleRates: List<Int>,
        val channelCounts: List<Int>,
        val timestamp: String,
    )

    /** Gather a fresh report (suspending; may do slow device IO). */
    suspend fun probe(): Report

    /** Single-line rendering for SfLogger / diagnostics chips. */
    fun formatForLog(r: Report): String
}

/**
 * G0 default implementation: conservative hardcoded values (no low-latency
 * claim, 44.1/48 kHz, stereo). Real AAudio probing arrives later.
 */
class DefaultAudioCapabilityProbe : AudioCapabilityProbe {

    override suspend fun probe(): Report = Report(
        osVersion = "Android " + Build.VERSION.RELEASE + " (SDK " + Build.VERSION.SDK_INT + ")",
        abi = Build.SUPPORTED_ABIS.firstOrNull() ?: "unknown",
        lowLatencySupported = false,
        sampleRates = listOf(44_100, 48_000),
        channelCounts = listOf(2),
        timestamp = isoTimestampUtc(),
    )

    override fun formatForLog(r: Report): String =
        "os=" + r.osVersion +
            " abi=" + r.abi +
            " lowLatency=" + r.lowLatencySupported +
            " sampleRates=" + r.sampleRates +
            " channelCounts=" + r.channelCounts +
            " at=" + r.timestamp

    private fun isoTimestampUtc(): String =
        SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply {
            timeZone = TimeZone.getTimeZone("UTC")
        }.format(Date())
}
