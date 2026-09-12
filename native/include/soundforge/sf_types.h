// SoundForge G0 — base C ABI types shared by all public headers.
// Result codes + log levels. No C++ types cross the ABI.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes returned by every C ABI function. */
typedef int32_t sf_result_t;
#define SF_OK 0
#define SF_E_INVALID_ARG 1
#define SF_E_NOT_FOUND 2
#define SF_E_SCHEMA 3
#define SF_E_VERSION 4
#define SF_E_IO 5
#define SF_E_NOMEM 6
#define SF_E_FILE_TOO_LARGE 7

/* Stable human-readable message for a result code. */
static const char* sf_error_string(int32_t code) {
  switch (code) {
    case SF_OK:
      return "ok";
    case SF_E_INVALID_ARG:
      return "invalid argument";
    case SF_E_NOT_FOUND:
      return "not found";
    case SF_E_SCHEMA:
      return "schema validation failed";
    case SF_E_VERSION:
      return "unsupported version";
    case SF_E_IO:
      return "i/o error";
    case SF_E_NOMEM:
      return "out of memory";
    case SF_E_FILE_TOO_LARGE:
      return "file too large";
    default:
      return "unknown error";
  }
}

/* Log levels. */
typedef enum sf_log_level {
  SF_LOG_DEBUG = 0,
  SF_LOG_INFO = 1,
  SF_LOG_WARN = 2,
  SF_LOG_ERROR = 3
} sf_log_level_t;

#ifdef __cplusplus
} /* extern "C" */
#endif
