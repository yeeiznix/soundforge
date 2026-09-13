// SoundForge G3 P2 — finite-block sample kernels (PLAN_G3 §4.2 D2 / §7.1).
// Kernel math, pan invariant + monotonicity, hard gate, bus accumulation,
// soft_limit linear zone / ceiling / finiteness / determinism.
#include <gtest/gtest.h>

#include "dsp_internal.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

// Deterministic test source: a fixed 1s-ish sine at 1 kHz in a 512-sample
// block (monic values designed to hit a range of magnitudes).
std::vector<float> sine_block(std::size_t n = sfcore::dsp::kBlockMaxSamples) {
  std::vector<float> b(n);
  for (std::size_t i = 0; i < n; ++i) {
    b[i] = static_cast<float>(std::sin(2.0 * sfcore::dsp::kPi * static_cast<double>(i) / 64.0));
  }
  return b;
}

bool all_finite(const std::vector<float>& b) {
  for (float v : b)
    if (!std::isfinite(v)) return false;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// apply_gain
// ---------------------------------------------------------------------------

TEST(DspKernel, ApplyGainScalesByHalf) {
  std::vector<float> b = sine_block(16);
  sfcore::dsp::apply_gain(b.data(), b.size(), 0.5f);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(b[i], 0.5f * static_cast<float>(std::sin(2.0 * sfcore::dsp::kPi * static_cast<double>(i) / 64.0)), 1e-7f);
  }
}

TEST(DspKernel, ApplyGainNegativeInvertsPolarity) {
  std::vector<float> b = sine_block(32);
  const std::vector<float> orig = b;
  sfcore::dsp::apply_gain(b.data(), b.size(), -1.0f);
  for (std::size_t i = 0; i < 32; ++i) EXPECT_FLOAT_EQ(b[i], -orig[i]);
}

TEST(DspKernel, ApplyGainZeroIsSilence) {
  std::vector<float> b = sine_block(32);
  sfcore::dsp::apply_gain(b.data(), b.size(), 0.0f);
  for (std::size_t i = 0; i < 32; ++i) EXPECT_FLOAT_EQ(b[i], 0.0f);
}

// ---------------------------------------------------------------------------
// apply_pan — equal-power law
// ---------------------------------------------------------------------------

TEST(DspKernel, ApplyPanSweepInvariantL2R2) {
  // L^2 + R^2 == source^2 across the full pan sweep (cos^2 + sin^2 = 1).
  for (int step = -10; step <= 10; ++step) {
    const float pan = step / 10.0f;
    std::vector<float> l = sine_block(64);
    std::vector<float> r = l;
    std::vector<float> ref = l;
    sfcore::dsp::apply_pan(l.data(), r.data(), l.size(), pan);
    for (std::size_t i = 0; i < 64; ++i) {
      const double e = static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
      EXPECT_NEAR(e, static_cast<double>(ref[i]) * ref[i], 1e-5)
          << "pan=" << pan << " i=" << i;
    }
  }
}

TEST(DspKernel, ApplyPanMonotonicSweep) {
  // pan -1..+1: L coefficient strictly decreasing, R strictly increasing.
  float prev_l = 1.01f, prev_r = -0.01f;
  for (int step = -10; step <= 10; ++step) {
    const float pan = step / 10.0f;
    std::vector<float> l(1, 1.0f);
    std::vector<float> r(1, 1.0f);
    sfcore::dsp::apply_pan(l.data(), r.data(), 1, pan);
    EXPECT_LE(l[0], prev_l) << "pan=" << pan;
    EXPECT_GE(r[0], prev_r) << "pan=" << pan;
    prev_l = l[0];
    prev_r = r[0];
  }
}

TEST(DspKernel, ApplyPanHardPans) {
  // Hard pan -1 -> {L=g, R=0}; +1 -> {L=0, R=g}. g is the input amplitude.
  // cos(pi/2)/sin(pi/2) are within ~1e-16 of the ideal 0/1 in float — assert
  // with tolerance (G3-3: no bit-exact cross-platform claims).
  std::vector<float> l = sine_block(64);
  std::vector<float> r = l;
  const std::vector<float> ref = l;
  sfcore::dsp::apply_pan(l.data(), r.data(), 64, -1.0f);
  for (std::size_t i = 0; i < 64; ++i) {
    EXPECT_NEAR(l[i], ref[i], 1e-5f);
    EXPECT_NEAR(r[i], 0.0f, 1e-5f);
  }
  l = sine_block(64);
  r = l;
  sfcore::dsp::apply_pan(l.data(), r.data(), 64, 1.0f);
  for (std::size_t i = 0; i < 64; ++i) {
    EXPECT_NEAR(l[i], 0.0f, 1e-5f);
    EXPECT_NEAR(r[i], ref[i], 1e-5f);
  }
}

TEST(DspKernel, ApplyPanCenterEqualPower) {
  // pan 0 -> cos(pi/4) = sin(pi/4): both channels = g/sqrt(2).
  std::vector<float> l(4, 1.0f);
  std::vector<float> r(4, 1.0f);
  sfcore::dsp::apply_pan(l.data(), r.data(), 4, 0.0f);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_NEAR(l[i], 0.70710678f, 1e-6f);
    EXPECT_NEAR(r[i], 0.70710678f, 1e-6f);
  }
}

// ---------------------------------------------------------------------------
// apply_gate — hard gate
// ---------------------------------------------------------------------------

TEST(DspKernel, ApplyGateOffIsSilence) {
  std::vector<float> b = sine_block(32);
  sfcore::dsp::apply_gate(b.data(), b.size(), 0.0f);
  for (std::size_t i = 0; i < 32; ++i) EXPECT_FLOAT_EQ(b[i], 0.0f);
}

TEST(DspKernel, ApplyGateOnIsPassthrough) {
  std::vector<float> b = sine_block(32);
  const std::vector<float> ref = b;
  sfcore::dsp::apply_gate(b.data(), b.size(), 1.0f);
  for (std::size_t i = 0; i < 32; ++i) EXPECT_FLOAT_EQ(b[i], ref[i]);
}

TEST(DspKernel, ApplyGateBoundary) {
  std::vector<float> b = sine_block(8);
  sfcore::dsp::apply_gate(b.data(), b.size(), 0.4999f);  // below 0.5 -> off
  for (std::size_t i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(b[i], 0.0f);

  std::vector<float> c = sine_block(8);
  const std::vector<float> ref = c;
  sfcore::dsp::apply_gate(c.data(), c.size(), 0.5f);  // at 0.5 -> on
  for (std::size_t i = 0; i < 8; ++i) EXPECT_FLOAT_EQ(c[i], ref[i]);
}

// ---------------------------------------------------------------------------
// mix_bus — accumulation
// ---------------------------------------------------------------------------

TEST(DspKernel, MixBusAccumulates) {
  std::vector<float> out(16, 0.0f);
  std::vector<float> a = sine_block(16);
  std::vector<float> b = sine_block(16);
  sfcore::dsp::mix_bus(out.data(), a.data(), 16);
  sfcore::dsp::mix_bus(out.data(), b.data(), 16);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_NEAR(out[i], a[i] + b[i], 1e-6f);
  }
}

TEST(DspKernel, MixBusStartsFromZero) {
  std::vector<float> out(16, 0.0f);
  std::vector<float> a = sine_block(16);
  sfcore::dsp::mix_bus(out.data(), a.data(), 16);
  // First accumulation onto a zero bus == the input exactly.
  for (std::size_t i = 0; i < 16; ++i) EXPECT_FLOAT_EQ(out[i], a[i]);
}

// ---------------------------------------------------------------------------
// soft_limit — tanh knee + hard ceiling
// ---------------------------------------------------------------------------

TEST(DspKernel, SoftLimitIdentityInLinearZone) {
  // |x| <= ceiling*0.5 -> unchanged (small-signal linear zone).
  std::vector<float> b = sine_block(16);
  sfcore::dsp::apply_gain(b.data(), b.size(), 0.4f);  // |x| <= 0.4 < 0.5
  const std::vector<float> ref = b;
  sfcore::dsp::soft_limit(b.data(), b.size(), 1.0f);
  for (std::size_t i = 0; i < 16; ++i) EXPECT_FLOAT_EQ(b[i], ref[i]);
}

TEST(DspKernel, SoftLimitCeilingExact) {
  // Output never exceeds ceiling, even for hot inputs.
  for (std::size_t n : std::vector<std::size_t>{1, 16, sfcore::dsp::kBlockMaxSamples}) {
    std::vector<float> b(n, 2.0f);  // well past ceiling
    sfcore::dsp::soft_limit(b.data(), b.size(), 1.0f);
    for (std::size_t i = 0; i < n; ++i) {
      EXPECT_LE(b[i], 1.0f);
      EXPECT_GE(b[i], -1.0f);
    }
  }
}

TEST(DspKernel, SoftLimitApproachesCeiling) {
  // A 10x-over-ceiling input pins the output at the ceiling (tanh saturates).
  std::vector<float> b(16, 10.0f);
  sfcore::dsp::soft_limit(b.data(), b.size(), 1.0f);
  for (std::size_t i = 0; i < 16; ++i) EXPECT_NEAR(b[i], 1.0f, 1e-3f);
}

TEST(DspKernel, SoftLimitFiniteForHugeInputs) {
  // ±1e30 and other huge-but-finite inputs must stay finite (no NaN/Inf;
  // tanh saturates, never overflows).
  std::vector<float> b{1.0e30f, -1.0e30f, 3.0e38f, -3.0e38f, 123.456f, -0.001f};
  sfcore::dsp::soft_limit(b.data(), b.size(), 1.0f);
  EXPECT_TRUE(all_finite(b));
  // And they are all bounded by the ceiling.
  for (float v : b) {
    EXPECT_TRUE(v <= 1.0f + 1e-6f);
    EXPECT_TRUE(v >= -1.0f - 1e-6f);
  }
}

TEST(DspKernel, SoftLimitNoNanInfForAnyFiniteInput) {
  std::vector<float> b = sine_block(sfcore::dsp::kBlockMaxSamples);
  // Scale across a wide deterministic range, run, and check all-finite.
  for (float g : {0.001f, 0.5f, 1.0f, 2.0f, 100.0f, 1.0e30f}) {
    std::vector<float> c = b;
    sfcore::dsp::apply_gain(c.data(), c.size(), g);
    sfcore::dsp::soft_limit(c.data(), c.size(), 1.0f);
    EXPECT_TRUE(all_finite(c)) << "gain=" << g;
  }
}

TEST(DspKernel, SoftLimitDeterministicDoubleRun) {
  std::vector<float> a = sine_block(sfcore::dsp::kBlockMaxSamples);
  std::vector<float> b = a;
  sfcore::dsp::soft_limit(a.data(), a.size(), 1.0f);
  sfcore::dsp::soft_limit(b.data(), b.size(), 1.0f);
  float max_diff = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) {
    max_diff = std::max(max_diff, std::fabs(a[i] - b[i]));
  }
  // Deterministic per platform: two identical runs agree within 1e-12.
  EXPECT_LE(max_diff, 1e-12f);
}