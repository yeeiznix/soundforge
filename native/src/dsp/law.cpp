// SoundForge G3 P2 — mixing law (PLAN_G3 §4.1 D1).
// Pure helpers: no locks, no memory allocation, no external includes beyond
// dsp_internal.hpp + math headers. Deterministic (same inputs -> same outputs;
// no data-dependent branches on floating flags).
#include <cmath>
#include <cstddef>

#include "dsp_internal.hpp"

namespace sfcore::dsp {

// ---------------------------------------------------------------------------
// Coherent peak: SUM |g| over all routed sources (worst case: full phase).
// ---------------------------------------------------------------------------
double peak_gain_lin(std::span<const double> sources) {
  double sum = 0.0;
  for (const double g : sources) sum += std::fabs(g);
  return sum;
}

// ---------------------------------------------------------------------------
// Incoherent power: sqrt(SUM g^2).
// ---------------------------------------------------------------------------
double power_gain_lin(std::span<const double> sources) {
  double sum_sq = 0.0;
  for (const double g : sources) sum_sq += g * g;
  return std::sqrt(sum_sq);
}

// ---------------------------------------------------------------------------
// Headroom: -20*log10(peak). peak <= 0 -> invalid (empty routes -> peak 0 ->
// -Inf -> +Inf dB, no meaningful number; JSON report writes null, D1 verdict).
// ---------------------------------------------------------------------------
MaybeHeadroom headroom_db(double peak) {
  if (peak <= 0.0) return MaybeHeadroom{};
  return MaybeHeadroom{-20.0 * std::log10(peak), true};
}

// ---------------------------------------------------------------------------
// Clip gate: peak > 1.0 <==> headroom_db(peak) < 0 (log10 strictly increasing).
// ---------------------------------------------------------------------------
bool clipped(double peak) { return peak > 1.0; }

// ---------------------------------------------------------------------------
// merge_law — D1 single call site (PLAN_G3 §4.1 / §6 P3): composes the four
// functions above for one output's route-gain vector. evaluate_mixer
// (routing.cpp) is the only caller; the render path shares this same law.
// ---------------------------------------------------------------------------
MergeLaw merge_law(std::span<const double> sources) {
  MergeLaw m;
  m.peak = peak_gain_lin(sources);
  m.power = power_gain_lin(sources);
  m.headroom = headroom_db(m.peak);
  m.clipped = clipped(m.peak);
  return m;
}

}  // namespace sfcore::dsp