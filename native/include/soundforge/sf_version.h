// SoundForge G0 — engine/schema version constants and compatibility query.
// engineVersion: SemVer of the binary that wrote a file (informational).
// schemaVersion: integer gate for open/migrate (monotonic, per docs/PLAN_G0.md §5).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SF_ENGINE_VERSION_MAJOR 0
#define SF_ENGINE_VERSION_MINOR 1
#define SF_ENGINE_VERSION_PATCH 0
#define SF_ENGINE_VERSION_SUFFIX "-g0"
#define SF_SCHEMA_VERSION 1

/* Default engine version string derived from the components above.
   The compiled-in stamp (version_gen.h) may override via -DSF_BUILD_VERSION. */
#define SF_STR_(x) #x
#define SF_STR(x)  SF_STR_(x)
#define SF_ENGINE_VERSION \
  SF_STR(SF_ENGINE_VERSION_MAJOR) "." SF_STR(SF_ENGINE_VERSION_MINOR) "." \
  SF_STR(SF_ENGINE_VERSION_PATCH) SF_ENGINE_VERSION_SUFFIX

/* Static string, e.g. "0.1.0-g0" (build stamp via CMake version_gen.h). */
const char* sf_engine_version(void);

/* Current canonical schema version, e.g. 1. */
int32_t sf_schema_version(void);

/* 1 if a project with this schemaVersion can be opened (natively or via migration). */
int32_t sf_is_compatible(int32_t schema_version);

#ifdef __cplusplus
} /* extern "C" */
#endif
