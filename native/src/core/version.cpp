// SoundForge G0 — engine/schema version implementation.
#include "soundforge/sf_version.h"

#if __has_include("version_gen.h")
#include "version_gen.h"
#else
#define SF_VERSION_STRING "0.1.0-g1"
#endif

extern "C" const char* sf_engine_version(void) { return SF_VERSION_STRING; }

extern "C" int32_t sf_schema_version(void) { return SF_SCHEMA_VERSION; }

extern "C" int32_t sf_is_compatible(int32_t schema_version) {
  return (schema_version >= 0 && schema_version <= SF_SCHEMA_VERSION) ? 1 : 0;
}
