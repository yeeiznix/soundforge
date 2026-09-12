// SoundForge G0 — diagnostics C ABI: crash-safe ring logging, error accessors,
// log sink, flush-to-file, and the startup capability probe stub (§6).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "soundforge/sf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sf_project_s sf_project_t;

/* Log one line into the 64 KB ring buffer; forwarded to the sink if set.
 * Lines are "[ISO8601][LEVEL][tag] msg", truncated to 512 chars of payload.
 */
void sf_log(sf_log_level_t level, const char* tag, const char* msg);

/* Sink receives every log line (never NULL pointers). NULL restores default. */
typedef void (*sf_log_sink_fn)(sf_log_level_t level, const char* tag, const char* msg);
void sf_set_log_sink(sf_log_sink_fn sink);

/* Append ring contents to `path` (created/appended; drains the ring).
 * G0 limitation: best-effort fopen/fwrite, not async-signal-safe (documented).
 */
sf_result_t sf_flush_logs(const char* path);

/* Startup capability probe — G0 stub (§6.1): hardcoded values, no HAL touch.
 * Writes JSON into buf: osVersion/abi "unknown", lowLatencySupported=false,
 * sampleRates=[44100,48000], channelCounts=[2], timestamp, engineVersion.
 */
sf_result_t sf_capability_probe_json(char* buf, size_t cap);

/* Free strings returned by sf_project_to_json. */
void sf_free_string(char* s);

/* Error accessors (declared in sf_project.h too). */
const char* sf_last_error(const sf_project_t* p);
const char* sf_last_error_global(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
