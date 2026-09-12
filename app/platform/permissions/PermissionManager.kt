// SoundForge G0 — runtime permission abstraction (stubbed implementation).
package id.soundforge.pastudio.platform.permissions

/** Permissions the shell will eventually request at runtime. */
enum class Permission {
    RECORD_AUDIO,
    BLUETOOTH_CONNECT,
    READ_EXTERNAL_STORAGE,
}

/**
 * Source of truth for permission state + user-facing rationale strings.
 * A real implementation queries ContextCompat and drives request dialogs.
 */
interface PermissionManager {

    /** Whether permission [p] is currently granted. */
    fun isGranted(p: Permission): Boolean

    /** Human-readable "why do you need this?" text shown before requesting. */
    fun rationale(p: Permission): String
}

/**
 * G0 stub: reports nothing as granted. Wired to the real permission APIs in a
 * later milestone; keeps UI code testable without a device.
 */
class StubPermissionManager : PermissionManager {

    override fun isGranted(p: Permission): Boolean = false

    override fun rationale(p: Permission): String = when (p) {
        Permission.RECORD_AUDIO ->
            "SoundForge uses the microphone to play test signals and capture impulse " +
                "responses, so it can measure how your room and sound system behave."
        Permission.BLUETOOTH_CONNECT ->
            "Bluetooth connect permission lets SoundForge pair with nearby measurement " +
                "microphones and audio interfaces during a session."
        Permission.READ_EXTERNAL_STORAGE ->
            "Storage read access lets SoundForge import measurement files and project " +
                "documents saved outside the app."
    }
}
