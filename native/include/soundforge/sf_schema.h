// SoundForge G0 — structural project-JSON validation.
// G0 validation is structural (required keys, types, UUID format, timestamps,
// closed top-level key set) mirroring native/data/schemas/project_schema.json.
// Full Draft 2020-12 validation runs in tests via Python/jsonschema.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "soundforge/sf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate a serialized project document.
 * Returns SF_OK when valid; SF_E_SCHEMA when invalid; SF_E_INVALID_ARG on null args.
 * On invalid JSON, err_buf receives a human-readable message (NUL-terminated).
 */
sf_result_t sf_validate_project_json(const char* json, size_t len, char* err_buf, size_t err_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
