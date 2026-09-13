// SoundForge G3 P2 — sfdsp internal header: mixing law + finite-block kernels.
// PLAN_G3 §4.1 (D1), §4.2 (D2), §7.1. NOT part of the public C ABI — no
// exports, no JNI (the ABI surface grows only by sf_scene_rename, 25 -> 26).
//
// Pure C++20, namespace sfcore::dsp. Leaf module: no locks, no memory
// allocation, no I/O. Deterministic PER PLATFORM; bit-exact cross-platform
// equality is NOT claimed (residual G3-3) — tests assert with tolerances.
//
// Naming: the law functions below are the D1 names used by evaluate_mixer in
// P3 (peak_gain_lin / power_gain_lin / headroom_db / clipped); the plan §7.1
// table calls the first two coherent_sum / power_sum — same math.
#pragma once

#include <cstddef>
#include <span>

namespace sfcore::dsp {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Max samples in one render block (D2 / PLAN_G3 §4.2). Kernels take an
// explicit `n` and are not bound to this cap; the chain-render harness (P3)
// chunks its input at this size.
constexpr std::size_t kBlockMaxSamples = 512;

// Double-precision pi (kernel pan + any DSP trig).
constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Sample-processing function type aliases (kernels below all match these)
// ---------------------------------------------------------------------------
using MonoKernel = void (*)(float* buf, std::size_t n, float param);
using StereoKernel = void (*)(float* buf_l, float* buf_r, std::size_t n,
                              float param);
using MixKernel = void (*)(float* out, const float* in, std::size_t n);

// ---------------------------------------------------------------------------
// AudioBlock — finite block the P3 chain-render harness pushes through nodes
// (D2). Stereo interleaving is the harness's concern; kernels stay mono.
// ---------------------------------------------------------------------------
struct AudioBlock {
  float L[kBlockMaxSamples];
  float R[kBlockMaxSamples];
  std::size_t n = 0;
  double gainLin = 1.0;  // node mixer linear gain: 10^(gainDb/20)
};

// ---------------------------------------------------------------------------
// Mixing law (D1 — replaces the G2-5 static desk estimate)
// ---------------------------------------------------------------------------
// Coherent worst case: SUM over ALL routed sources of |g| (all in phase).
// For one source this equals the old G2 peakGainLin max — single-source
// invariance (oracle R-A): every fixture/test graph is single-source, so the
// G2 numbers are preserved byte-for-byte.
double peak_gain_lin(std::span<const double> sources);

// Incoherent/uncorrelated estimate: sqrt(SUM g^2). For gains >= 0:
//   max(g) <= power_gain_lin <= peak_gain_lin
// so the two keys bracket the true level between full correlation and
// uncorrelated energy. Negative gains count by magnitude in both sums.
double power_gain_lin(std::span<const double> sources);

// Headroom below the clip line: -20*log10(peak). The peak <= 0 case is
// invalid (empty routes -> peak 0 -> -20*log10(0) = +Inf, which nlohmann
// would dump implementation-defined) — represented by valid=false so the
// JSON report (P3) can write null instead. `number|null` on the wire.
//
// clipped(peak) <==> headroom_db(peak).db < 0 is a pure refactor of the G2
// threshold peak > 1.0: log10 is strictly increasing, so -20*log10(peak) < 0
// iff peak > 1.0.
struct MaybeHeadroom {
  double db = 0.0;    // dB below clip; meaningful only when valid
  bool valid = false; // false <=> peak <= 0 (no routes / silence)
};
MaybeHeadroom headroom_db(double peak);

// Law-derived clip gate: peak > 1.0 (same predicate as the G2 desk estimate).
bool clipped(double peak);

// Per-channel-bound property (oracle R-A(c), PLAN_G3 §4.2 verdict note):
// the scalar law never *understates* per-channel clip risk. Equal-power pan
// bounds each channel by the scalar gain (cos^2 theta, sin^2 theta <= 1), so
// peak_gain_lin/power_gain_lin are always an upper bound for any single
// channel's true peak. Revisited at G4 when the render path reports
// per-channel true peaks.

// ---------------------------------------------------------------------------
// Finite-block sample kernels (D2)
// ---------------------------------------------------------------------------
// All loops iterate over float* with an explicit n; prefetch-friendly, no
// alignment requirements beyond natural. apply_gain/gate/soft_limit operate
// in place; mix_bus accumulates into out. Deterministic, no NaN/Inf for any
// finite input, no allocation, no locks (the "G0 invariant").

// Scale every sample by a linear gain (10^(gainDb/20)).
void apply_gain(float* buf, std::size_t n, float gain_lin);

// Equal-power pan law: theta = pi/4 * (pan + 1), pan in [-1, 1];
// L = cos(theta), R = sin(theta) -> L^2 + R^2 == 1 across the sweep.
// Hard pan -1 -> {L=g, R=0}, +1 -> {L=0, R=g}. Pan is clamped to [-1, 1]
// defensively (contract is already [-1, 1]).
void apply_pan(float* buf_l, float* buf_r, std::size_t n, float pan);

// Hard gate: gain 0.0 when gate < 0.5, 1.0 otherwise (mute/solo role).
void apply_gate(float* buf, std::size_t n, float gate);

// Accumulate a bus input into out: out[i] += in[i].
void mix_bus(float* out, const float* in, std::size_t n);

// Soft limit: identity in the linear zone (|x| <= ceiling*0.5), then a smooth
// tanh knee saturating flat at the ceiling. Output never exceeds ceiling for
// any finite input; finite output for any finite input; deterministic.
void soft_limit(float* buf, std::size_t n, float ceiling);

}  // namespace sfcore::dsp