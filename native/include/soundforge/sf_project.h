// SoundForge G0 — project lifecycle C ABI (Kotlin ↔ C++ bridge surface).
//
// Contract (docs/PLAN_G0.md §4):
//  - Stable C ABI: C types only, no exceptions across the boundary.
//  - Opaque handles: Kotlin holds jlong; 0/NULL means no project.
//  - Native is NOT thread-safe per-handle; Kotlin calls from Dispatchers.IO.
//  - Every call reports failure via SF_* code; human message via
//    sf_last_error()/sf_last_error_global().
//  - sf_project_save_to_path appends a project.save audit entry and updates
//    modifiedAt. sf_project_to_json does NOT (documented decision, §5.3).
//  - G0: synchronous calls; G2 introduces a command queue for audio safety.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "soundforge/sf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sf_project_s sf_project_t; /* opaque; internally soundforge::SfProject */

/* --- Version (re-exported from sf_version.h for bridge convenience) --- */
const char* sf_engine_version(void);
int32_t     sf_schema_version(void);
int32_t     sf_is_compatible(int32_t schema_version);

/* --- Lifecycle --- */
sf_project_t* sf_project_create(const char* name, const char* author); /* author may be NULL */
void          sf_project_destroy(sf_project_t* p);
sf_result_t   sf_project_clone(const sf_project_t* src, sf_project_t** out);

/* --- JSON codec (caller owns returned buffers; free with sf_free_string) --- */
sf_result_t sf_project_to_json(const sf_project_t* p, char** out_json, size_t* out_len);
sf_result_t sf_project_from_json(const char* json, size_t len, sf_project_t** out);
sf_result_t sf_project_save_to_path(const sf_project_t* p, const char* path);
sf_result_t sf_project_open_from_path(const char* path, sf_project_t** out);

/* --- Accessors (G0 minimal; NULL handle yields "") --- */
const char* sf_project_get_name(const sf_project_t* p);
const char* sf_project_get_id(const sf_project_t* p);
int32_t     sf_project_get_schema_version(const sf_project_t* p);
const char* sf_project_get_engine_version(const sf_project_t* p);

/* --- G1 editing mutators (schema v2: venue.dimensions, scene.geometry) ---
 * Additive ABI (PLAN_G1 §4.2). Every mutator appends an audit entry
 * (actor "user", action project.rename|venue.update|scene.update) and bumps
 * project.modifiedAt. Errors: SF_* code + message via sf_last_error(handle).
 * Names are non-empty, at most 200 chars. */
sf_result_t sf_project_rename(sf_project_t* p, const char* new_name);
sf_result_t sf_venue_rename(sf_project_t* p, const char* new_name);
sf_result_t sf_venue_set_dimensions(sf_project_t* p, double width_m, double depth_m,
                                    double height_m);
sf_result_t sf_scene_set_geometry(sf_project_t* p, double cx, double cy, double cz,
                                  double lx, double ly, double lz);

/* --- Health ---
 * Writes a JSON report:
 *   {"status":"ok"|"warning"|"error","warnings":[...],"errors":[...],"stats":{...}}
 * Returns SF_OK when status=="ok"; SF_E_SCHEMA when warnings/errors exist;
 * SF_E_NOMEM when report_cap is insufficient. Recommended cap: 8192.
 */
sf_result_t sf_project_health_check(const sf_project_t* p, char* report_buf, size_t report_cap);

/* --- Diagnostics / error reporting --- */
void        sf_free_string(char* s);
const char* sf_last_error(const sf_project_t* p); /* p!=NULL: handle error; NULL: thread-local */
const char* sf_last_error_global(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
