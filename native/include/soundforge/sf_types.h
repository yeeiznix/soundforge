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
