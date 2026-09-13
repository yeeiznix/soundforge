// SoundForge G3 P2 — mixing law unit tests (PLAN_G3 §4.1 D1 / §6 P2).
// Generators: coherent peak sum (peak_gain_lin), incoherent power sum
// (power_gain_lin), headroom (headroom_db, null-on-empty intent), clip gate
// (clipped). Write-only negative gains assert by magnitude.
#include <gtest/gtest.h>

#include "dsp_internal.hpp"

#include <array>
#include <cmath>
#include <vector>

namespace {

sfcore::dsp::MaybeHeadroom hr(double peak) { return sfcore::dsp::headroom_db(peak); }

}  // namespace

// ---------------------------------------------------------------------------
// peak_gain_lin — coherent sum
// ---------------------------------------------------------------------------

TEST(DspLaw, PeakGainLinSingleSourceInvariance) {
  // One source: Σ|g| == the source gain exactly (G2's old max, byte-compatible).
  EXPECT_DOUBLE_EQ(sfcore::dsp::peak_gain_lin(std::array<double, 1>{1.414}), 1.414);
  EXPECT_DOUBLE_EQ(sfcore::dsp::peak_gain_lin(std::array<double, 1>{0.2512}), 0.2512);
  EXPECT_DOUBLE_EQ(sfcore::dsp::peak_gain_lin(std::array<double, 1>{3.981}),
                   3.981);
}

TEST(DspLaw, PeakGainLinTwoSourcesSum) {
  const std::array<double, 2> both_one{1.0, 1.0};
  EXPECT_DOUBLE_EQ(sfcore::dsp::peak_gain_lin(both_one), 2.0);
  const std::array<double, 3> mix{0.5, 1.5, 2.0};
  EXPECT_DOUBLE_EQ(sfcore::dsp::peak_gain_lin(mix), 4.0);
}

TEST(DspLaw, PeakGainLinEmptySetIsZero) {
  const std::array<double, 0> empty{};
  EXPECT_DOUBLE_EQ(sfcore::dsp::peak_gain_lin(empty), 0.0);
}

TEST(DspLaw, PeakGainLinNegativeGainsByMagnitude) {
  // Coherent worst case counts |g|: sign must not cancel the sum.
  const std::array<double, 2> mixed{-1.0, 1.0};
  EXPECT_DOUBLE_EQ(sfcore::dsp::peak_gain_lin(mixed), 2.0);
}

// ---------------------------------------------------------------------------
// power_gain_lin — incoherent power sum
// ---------------------------------------------------------------------------

TEST(DspLaw, PowerGainLinSingleSource) {
  EXPECT_DOUBLE_EQ(sfcore::dsp::power_gain_lin(std::array<double, 1>{1.414}),
                   1.414);
  EXPECT_DOUBLE_EQ(sfcore::dsp::power_gain_lin(std::array<double, 1>{0.2512}),
                   0.2512);
}

TEST(DspLaw, PowerGainLinRootSumSquares) {
  const std::array<double, 2> both_one{1.0, 1.0};
  EXPECT_NEAR(sfcore::dsp::power_gain_lin(both_one), std::sqrt(2.0), 1e-12);
  // sqrt(1^2 + 2^2) = sqrt(5)
  const std::array<double, 2> one_two{1.0, 2.0};
  EXPECT_NEAR(sfcore::dsp::power_gain_lin(one_two), std::sqrt(5.0), 1e-12);
}

TEST(DspLaw, PowerGainLinMixedSigns) {
  // sqrt(a^2 + b^2) is sign-invariant (incoherent energy).
  const std::array<double, 2> posneg{3.0, -4.0};
  EXPECT_NEAR(sfcore::dsp::power_gain_lin(posneg), 5.0, 1e-12);
  const std::array<double, 2> bothneg{-3.0, -4.0};
  EXPECT_NEAR(sfcore::dsp::power_gain_lin(bothneg), 5.0, 1e-12);
}

TEST(DspLaw, PowerGainLinEmptySetIsZero) {
  const std::array<double, 0> empty{};
  EXPECT_DOUBLE_EQ(sfcore::dsp::power_gain_lin(empty), 0.0);
}

TEST(DspLaw, PowerBracketsPeak) {
  // max <= powerGainLin <= peakGainLin for non-negative gains (§4.1).
  const std::array<double, 4> gains{0.5, 1.0, -2.0, 3.0};
  const double peak = sfcore::dsp::peak_gain_lin(gains);
  const double power = sfcore::dsp::power_gain_lin(gains);
  EXPECT_LE(power, peak);
  EXPECT_GE(power, 3.0);  // max magnitude of the set {0.5, 1.0, -2.0, 3.0}
  // power = sqrt(0.25 + 1 + 4 + 9) = sqrt(14.25) ~ 3.775 < 6.5
  EXPECT_NEAR(power, std::sqrt(14.25), 1e-12);
}

// ---------------------------------------------------------------------------
// headroom_db
// ---------------------------------------------------------------------------

TEST(DspLaw, HeadroomDbAtUnityIsZero) {
  const sfcore::dsp::MaybeHeadroom h = hr(1.0);
  EXPECT_TRUE(h.valid);
  EXPECT_NEAR(h.db, 0.0, 1e-12);
}

TEST(DspLaw, HeadroomDbMinusSixAtDouble) {
  // -20*log10(2) = -6.0206
  const sfcore::dsp::MaybeHeadroom h = hr(2.0);
  EXPECT_TRUE(h.valid);
  EXPECT_NEAR(h.db, -6.0205999132796242, 1e-9);
}

TEST(DspLaw, HeadroomDbPlusSixAtHalf) {
  // -20*log10(0.5) = +6.0206
  const sfcore::dsp::MaybeHeadroom h = hr(0.5);
  EXPECT_TRUE(h.valid);
  EXPECT_NEAR(h.db, 6.0205999132796242, 1e-9);
}

TEST(DspLaw, HeadroomDbNullOnNoRoutes) {
  // peak 0 (empty routes) -> invalid: -20*log10(0) = +Inf is not a number;
  // JSON report (P3) writes null (D1 verdict). 
  const sfcore::dsp::MaybeHeadroom h = hr(0.0);
  EXPECT_FALSE(h.valid);
}

// ---------------------------------------------------------------------------
// clipped — law-derived clip gate
// ---------------------------------------------------------------------------

TEST(DspLaw, ClippedAtThresholdAndAbove) {
  EXPECT_FALSE(sfcore::dsp::clipped(1.0));   // == threshold: not clipped
  EXPECT_TRUE(sfcore::dsp::clipped(1.0001)); // just above: clipped
  EXPECT_TRUE(sfcore::dsp::clipped(2.0));    // +6 dB: clipped
}

TEST(DspLaw, ClippedBelowAltAndZero) {
  EXPECT_FALSE(sfcore::dsp::clipped(0.5));
  EXPECT_FALSE(sfcore::dsp::clipped(0.0));  // no routes: not clipped
}

TEST(DspLaw, ClippedEquivalentToHeadroomNegative) {
  // clipped(peak) <==> headroom_db(peak).db < 0 (pure refactor of G2's
  // peak > 1.0; log10 strictly increasing). Verify the equivalence at a
  // fine sweep, hitting both sides of the threshold.
  for (double peak = 0.25; peak <= 4.0; peak += 0.125) {
    const bool law = sfcore::dsp::clipped(peak);
    const bool hr_equiv = peak > 0.0 && hr(peak).db < 0.0;
    EXPECT_EQ(law, hr_equiv) << "peak=" << peak;
  }
}

// ---------------------------------------------------------------------------
// merge_law — D1 single call site (PLAN_G3 §4.1 / §6 P3: routing.cpp calls
// the ENTIRE law through merge_law; the render path shares the same code).
// ---------------------------------------------------------------------------

TEST(DspLaw, MergeLawMatchesComposition) {
  // merge_law must be exactly the four law functions composed — same values,
  // same null-on-empty and clip semantics (evaluate_mixer's numbers depend on
  // byte-for-byte equality with the G2 single-source values).
  const std::array<double, 4> gains{0.5, 1.0, -2.0, 3.0};
  const sfcore::dsp::MergeLaw m = sfcore::dsp::merge_law(gains);

  const double peak = sfcore::dsp::peak_gain_lin(gains);
  const double power = sfcore::dsp::power_gain_lin(gains);
  const sfcore::dsp::MaybeHeadroom h = sfcore::dsp::headroom_db(peak);

  EXPECT_DOUBLE_EQ(m.peak, peak);
  EXPECT_DOUBLE_EQ(m.power, power);
  EXPECT_EQ(m.headroom.valid, h.valid);
  EXPECT_DOUBLE_EQ(m.headroom.db, h.db);
  EXPECT_EQ(m.clipped, sfcore::dsp::clipped(peak));

  // Sanity spot-checks of the values themselves.
  EXPECT_DOUBLE_EQ(m.peak, 6.5);            // |0.5|+|1|+|-2|+|3|
  EXPECT_NEAR(m.power, std::sqrt(14.25), 1e-12);
  EXPECT_TRUE(m.clipped);                   // peak 6.5 > 1.0
  EXPECT_TRUE(m.headroom.valid);
  EXPECT_NEAR(m.headroom.db, -20.0 * std::log10(6.5), 1e-12);
}

TEST(DspLaw, MergeLawEmptyRoutesNullHeadroom) {
  // Empty route-gain vector: peak 0 -> headroom invalid (JSON null), clip
  // gate off — the D1 no-routes posture.
  const std::array<double, 0> empty{};
  const sfcore::dsp::MergeLaw m = sfcore::dsp::merge_law(empty);
  EXPECT_DOUBLE_EQ(m.peak, 0.0);
  EXPECT_DOUBLE_EQ(m.power, 0.0);
  EXPECT_FALSE(m.headroom.valid);
  EXPECT_FALSE(m.clipped);
}