// SoundForge G1 — pure geometry kernels for venue/scene editing (PLAN_G1 §4.1).
// All kernels are pure and thread-safe: no handle, no global state. Errors
// funnel through set_last_error (thread-local + global) and return codes;
// never throw across the ABI (bodies wrapped in SF_CATCH_ERRORS).
#pragma once

#include <stdint.h>

#include "soundforge/sf_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validate a room box (meters). Returns SF_OK when width, depth, height are
 * all finite and > 0; SF_E_INVALID_ARG otherwise (last_error set). */
sf_result_t sf_geo_validate_box(double width, double depth, double height);

/* Point-in-room test with inclusive bounds: 0 <= x <= width, etc.
 * Returns SF_E_INVALID_ARG on a null out_inside or an invalid room box. */
sf_result_t sf_geo_point_in_box(double x, double y, double z,
                                double width, double depth, double height,
                                int* out_inside);

/* Euclidean distance (meters) between two points.
 * Returns SF_E_INVALID_ARG on a null out_m or non-finite inputs. */
sf_result_t sf_geo_distance(double x1, double y1, double z1,
                            double x2, double y2, double z2,
                            double* out_m);

/* Largest linear dimension of a room box (for venue summaries).
 * Returns SF_E_INVALID_ARG on a null out_m or an invalid room box. */
sf_result_t sf_geo_max_dimension(double width, double depth, double height,
                                 double* out_m);

#ifdef __cplusplus
} /* extern "C" */
#endif