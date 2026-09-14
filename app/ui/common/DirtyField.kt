// SoundForge G3 — reusable dirty-tracking helper for the new G3 fields
// (docs/PLAN_G3.md §4.7 D7, §6 P6). Applied only to the scene-name field and
// the per-node preset rows. Internal state uses Compose mutableStateOf so that
// reading .isDirty / .value inside a composable triggers recomposition on edit;
// editing in onClick handlers (chip taps, text changes) therefore updates the
// UI immediately — the dirty gate on commit buttons works at runtime, not just
// statically.
//
// The committed document stays authoritative: [seed] refreshes the field from
// it, but ONLY when the field is pristine — an in-progress dirty edit is never
// clobbered by a background refresh (the .isDirty flag gates seed writes).
//
// Contract (D7):
//  - [edit] records a user edit and marks the field dirty (interaction-based,
//    even when the value happens to equal the committed one).
//  - [commit] clears the dirty flag right after the caller handed the value to
//    the ViewModel; a later [seed] from the refreshed document then re-syncs
//    the canonical value — unless the user edited again in the meantime (the
//    seed silently no-ops, keeping the pending edit).
//  - [seed] only touches pristine fields: dirty fields keep local typing.
//
// Keep it minimal and idiomatic — pure view state backed by Compose snapshot.
package id.soundforge.pastudio.common

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * One editable field with seed/commit/dirty tracking.
 *
 * Create via `remember { DirtyField(initial) }` inside a composable.
 * Every property is backed by [mutableStateOf], so Composition reacts to
 * edits automatically.
 */
class DirtyField<T>(initial: T) {

    private var current: T by mutableStateOf(initial)
    private var dirty: Boolean by mutableStateOf(false)

    /** Current field content (local typing while dirty). */
    var value: T
        get() = current
        private set(newValue) { current = newValue }

    /** True while the user has an uncommitted edit. */
    val isDirty: Boolean get() = dirty

    /** True when the field matches the last seeded (committed) value. */
    val pristine: Boolean get() = !dirty

    /** Record a user edit; marks the field dirty even when the new value
     *  happens to equal the committed one (interaction-based, D7). */
    fun edit(newValue: T) {
        current = newValue
        dirty = true
    }

    /** Re-seed from the committed document; never clobbers a dirty edit. */
    fun seed(committed: T) {
        if (pristine) current = committed
    }

    /** Mark the field clean (call right after handing the value to the VM). */
    fun commit() {
        dirty = false
    }
}