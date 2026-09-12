// SoundForge G0 — project schema migration API.
// v0→v1 is the only migration path in G0; it is idempotent and injects
// defaults for all missing canonical keys (docs/PLAN_G0.md §5.5).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "soundforge/sf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Migrate a serialized project document in place.
 * json_inout: buffer of size `cap` holding `*inout_len` bytes of input (plus NUL).
 * On success: buffer rewritten (pretty, 2-space indent), *inout_len = new length.
 * Returns SF_OK, SF_E_VERSION (no path / downgrade), SF_E_SCHEMA (parse error),
 * SF_E_NOMEM (cap insufficient for output).
 * Idempotent: migrating an already-migrated document is a no-op.
 */
sf_result_t sf_migrate_json(char* json_inout, size_t* inout_len, size_t cap, int32_t from_ver, int32_t to_ver);

#ifdef __cplusplus
} /* extern "C" */
#endif
