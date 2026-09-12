package id.soundforge.pastudio.navigation

/**
 * Canonical navigation destinations for the G0 shell.
 *
 * The eight editor placeholders additionally accept an optional
 * "projectId" query argument, appended by NavGraph as
 * "<path>?projectId={projectId}".
 */
sealed class Route(val path: String) {
    /** Home dashboard (start destination). */
    object Home : Route("home")

    /** Stored .sfproj documents. */
    object ProjectList : Route("projectList")

    /** Scene editor placeholder. */
    object Scene : Route("scene")

    /** Signal editor placeholder. */
    object Signal : Route("signal")

    /** Mixer placeholder. */
    object Mixer : Route("mixer")

    /** DSP chain placeholder. */
    object Dsp : Route("dsp")

    /** Prediction placeholder. */
    object Prediction : Route("prediction")

    /** Measurement placeholder. */
    object Measurement : Route("measurement")

    /** Training placeholder. */
    object Training : Route("training")

    /** Reports placeholder. */
    object Reports : Route("reports")
}
