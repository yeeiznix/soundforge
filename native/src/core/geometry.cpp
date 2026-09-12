// SoundForge G1 — geometry kernels (PLAN_G1 §4.1).
// Pure functions, no global state; all error paths funnel through
// set_last_error (thread-local + global) and return SF_E_INVALID_ARG.
#include "sf_internal.hpp"

#include <cmath>

namespace sfcore {

// Room box validity: finite and strictly positive dimensions (meters).
bool geo_box_valid(double w, double d, double h) {
  return std::isfinite(w) && std::isfinite(d) && std::isfinite(h) &&
         w > 0.0 && d > 0.0 && h > 0.0;
}

}  // namespace sfcore

extern "C" sf_result_t sf_geo_validate_box(double width, double depth, double height) {
  try {
    if (!sfcore::geo_box_valid(width, depth, height)) {
      sfcore::set_last_error("validate_box: dimensions must be finite and > 0");
      return SF_E_INVALID_ARG;
    }
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_geo_point_in_box(double x, double y, double z,
                                           double width, double depth, double height,
                                           int* out_inside) {
  try {
    if (!out_inside) {
      sfcore::set_last_error("point_in_box: null argument");
      return SF_E_INVALID_ARG;
    }
    if (!sfcore::geo_box_valid(width, depth, height)) {
      sfcore::set_last_error("point_in_box: invalid room box");
      return SF_E_INVALID_ARG;
    }
    *out_inside = (x >= 0.0 && x <= width && y >= 0.0 && y <= depth &&
                   z >= 0.0 && z <= height)
                      ? 1
                      : 0;
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_geo_distance(double x1, double y1, double z1,
                                       double x2, double y2, double z2,
                                       double* out_m) {
  try {
    if (!out_m) {
      sfcore::set_last_error("distance: null argument");
      return SF_E_INVALID_ARG;
    }
    const double dx = x2 - x1;
    const double dy = y2 - y1;
    const double dz = z2 - z1;
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) {
      sfcore::set_last_error("distance: non-finite input");
      return SF_E_INVALID_ARG;
    }
    *out_m = std::sqrt(dx * dx + dy * dy + dz * dz);
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_geo_max_dimension(double width, double depth, double height,
                                            double* out_m) {
  try {
    if (!out_m) {
      sfcore::set_last_error("max_dimension: null argument");
      return SF_E_INVALID_ARG;
    }
    if (!sfcore::geo_box_valid(width, depth, height)) {
      sfcore::set_last_error("max_dimension: invalid room box");
      return SF_E_INVALID_ARG;
    }
    *out_m = std::fmax(width, std::fmax(depth, height));
    return SF_OK;
  } SF_CATCH_ERRORS()
}