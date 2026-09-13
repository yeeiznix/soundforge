// SoundForge G3 P2 — finite-block sample kernels (PLAN_G3 §4.2 D2).
// Leaf data-parallel functions: no locks, no function calls beyond math
// builtins, no memory allocation. All loops iterate float* with explicit n;
// prefetch-friendly, natural alignment only. Deterministic per platform
// (bit-exact cross-platform equality NOT claimed — residual G3-3).
#include <cmath>
#include <cstddef>

#include "dsp_internal.hpp"

namespace sfcore::dsp {

void apply_gain(float* buf, std::size_t n, float gain_lin) {
  for (std::size_t i = 0; i < n; ++i) buf[i] *= gain_lin;
}

void apply_pan(float* buf_l, float* buf_r, std::size_t n, float pan) {
  // Equal-power law: theta = pi/4 * (pan + 1). Clamp defensively so the
  // contract pan in [-1, 1] cannot produce out-of-range trig.
  double p = pan;
  if (p < -1.0) p = -1.0;
  if (p > 1.0) p = 1.0;
  const double theta = (p + 1.0) * 0.25 * kPi;
  const float gl = static_cast<float>(std::cos(theta));
  const float gr = static_cast<float>(std::sin(theta));
  for (std::size_t i = 0; i < n; ++i) {
    buf_l[i] *= gl;
    buf_r[i] *= gr;
  }
}

void apply_gate(float* buf, std::size_t n, float gate) {
  // Hard gate: 0.0 below 0.5, 1.0 otherwise.
  const float g = (gate < 0.5f) ? 0.0f : 1.0f;
  for (std::size_t i = 0; i < n; ++i) buf[i] *= g;
}

void mix_bus(float* out, const float* in, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) out[i] += in[i];
}

void soft_limit(float* buf, std::size_t n, float ceiling) {
  // Soft limiter: identity while |x| <= knee (= ceiling*0.5), then a smooth
  // tanh knee flattening asymptotically toward the ceiling. The output never
  // exceeds ceiling for any finite input, and tanh saturates so even huge
  // finite inputs (e.g. 1e30) stay finite. Ceiling == 0 (or negative) is a
  // degenerate "no signal" case -> silence (no NaN/Inf). Deterministic: the
  // same inputs produce bit-identical outputs on the same platform.
  if (ceiling <= 0.0f) {
    for (std::size_t i = 0; i < n; ++i) buf[i] = 0.0f;
    return;
  }
  const double knee = 0.5 * static_cast<double>(ceiling);
  const double reach = static_cast<double>(ceiling) - knee;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = buf[i];
    const double ax = std::fabs(x);
    if (ax <= knee) continue;  // linear zone: identity
    const double y = knee + reach * std::tanh((ax - knee) / reach);
    buf[i] = static_cast<float>(x < 0.0 ? -y : y);
  }
}

}  // namespace sfcore::dsp