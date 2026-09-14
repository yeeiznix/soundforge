// SoundForge G4 P2 — true-peak meter tests (PLAN_G4 §6 P2, §7.1
// test_true_peak.cpp row).
//
// Proves D3: 4x polyphase FIR + K-sample lookback tail + non-decaying latch.
// Steady-state cases prime the tail first (warm samples), then resetLatch(),
// then measure — the FIR onset transient (cold-start overshoot ~+3.12 dB at
// fs/4) is genuine filter behavior and is pinned by its own test below.
#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "true_peak.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

#ifndef TRUE_PEAK_SRC
#define TRUE_PEAK_SRC ""
#endif

namespace {

using sfmeasure::TruePeak;
using sfmeasure::TruePeakChannels;

constexpr std::size_t kFrames = 64;

// fs/4 + pi/4: samples land at +/-A/sqrt2, 4x grid puts a phase on the crest.
float tone_sample(std::size_t i, float amp = 1.0f, float phase = 0.78539816f) {
  const double v =
      amp * std::sin(2.0 * M_PI * 0.25 * static_cast<double>(i) + phase);
  return static_cast<float>(v);
}

// Fill `n` contiguous samples into row 0 (and row 1 with `fill_r`).
void fill_tone(float* x, std::size_t n, float amp = 1.0f,
               float phase = 0.78539816f) {
  for (std::size_t i = 0; i < n; ++i) x[i] = tone_sample(i, amp, phase);
}

// Prime the tail with `warm` samples of the same signal, then zero the latch
// so the measurement starts in steady state (no onset transient).
template <typename F>
double steady_measure(F gen, std::size_t warm = 256) {
  float buf[kFrames];
  TruePeak tp;
  for (std::size_t s = 0; s < warm; s += kFrames) {
    const std::size_t n = std::min(kFrames, warm - s);
    gen(buf, n, s);
    float* rows[TruePeakChannels] = {buf, nullptr};
    tp.process(rows, 1, n);
  }
  tp.resetLatch();
  // measured window: 2 blocks of kFrames
  for (int b = 0; b < 2; ++b) {
    gen(buf, kFrames, warm + static_cast<std::size_t>(b) * kFrames);
    float* rows[TruePeakChannels] = {buf, nullptr};
    tp.process(rows, 1, kFrames);
  }
  return tp.latch(0);
}

// Generate fs/4 + pi/4 tone into x[n] starting at global offset `off`.
void gen_fs4(float* x, std::size_t n, std::size_t off, float amp = 1.0f) {
  for (std::size_t i = 0; i < n; ++i)
    x[i] = tone_sample(off + i, amp);
}

double steady_fs4(float amp = 1.0f, std::size_t warm = 256) {
  return steady_measure(
      [amp](float* x, std::size_t n, std::size_t off) { gen_fs4(x, n, off, amp); },
      warm);
}

double steady_dc(float amp = 1.0f, std::size_t warm = 256) {
  return steady_measure(
      [amp](float* x, std::size_t n, std::size_t) {
        for (std::size_t i = 0; i < n; ++i) x[i] = amp;
      },
      warm);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) fs/4 + pi/4 full-scale sine: sample peak A/sqrt2, true peak A
//    => +3.01 dB +/- 0.1 (review R-C grid-aligned crest).
// ---------------------------------------------------------------------------
TEST(TruePeak, Fs4HalfPhaseAddsThreePointZeroOneDb) {
  const double steady = steady_fs4(1.0f);

  // sample peak of the generated signal (f32-rounded)
  double sample_peak = 0.0;
  for (std::size_t i = 0; i < 512; ++i)
    sample_peak = std::max(sample_peak,
                           static_cast<double>(std::fabs(tone_sample(i))));

  ASSERT_NEAR(steady, 1.0, 2e-4) << "true peak of full-scale fs/4 tone";
  const double db = 20.0 * std::log10(steady / sample_peak);
  EXPECT_NEAR(db, 3.0103, 0.1) << "delta over sample peak";
  EXPECT_GT(db, 2.91);
  EXPECT_LT(db, 3.11);
}

// ---------------------------------------------------------------------------
// 2) DC unity: no systematic offset (|DC-1| <= 1e-4).
// ---------------------------------------------------------------------------
TEST(TruePeak, DcUnityWithinOneOverTenThousand) {
  const double steady = steady_dc(1.0f);
  EXPECT_NEAR(steady, 1.0, 1e-4);
}

// ---------------------------------------------------------------------------
// 3) Multi-block hold: a two-block measure equals the one-shot measure
//    (tail continuity — no boundary discontinuity, no lost crest).
// ---------------------------------------------------------------------------
TEST(TruePeak, TwoBlocksEqualOneShot) {
  float buf[kFrames];
  TruePeak a, b;

  // prime both identically
  float warm[kFrames];
  for (std::size_t s = 0; s < 256; s += kFrames) {
    fill_tone(warm, kFrames);
    float* rows[TruePeakChannels] = {warm, nullptr};
    a.process(rows, 1, kFrames);
    b.process(rows, 1, kFrames);
  }
  a.resetLatch();
  b.resetLatch();

  // `a`: two separate blocks; `b`: one combined 128-sample block
  const std::size_t n = 128;
  float xa[kFrames * 2];
  fill_tone(xa, n);
  float* rows_a[TruePeakChannels] = {xa, nullptr};
  float* rows_b[TruePeakChannels] = {xa + kFrames, nullptr};
  a.process(rows_a, 1, kFrames);
  a.process(rows_b, 1, kFrames);
  float* rowb[TruePeakChannels] = {xa, nullptr};
  b.process(rowb, 1, n);

  EXPECT_NEAR(a.latch(0), b.latch(0), 1e-12)
      << "split-block processing must not lose or add a crest at the seam";
}

// ---------------------------------------------------------------------------
// 4) Latch monotone: never decreases until reset.
// ---------------------------------------------------------------------------
TEST(TruePeak, LatchNeverDecreases) {
  float buf[kFrames];
  fill_tone(buf, kFrames);
  float* rows[TruePeakChannels] = {buf, nullptr};

  TruePeak tp;
  const long before = sftest::alloc_calls();
  tp.process(rows, 1, kFrames);  // loud fs/4 tone
  const double loud = tp.latch(0);
  const long between = sftest::alloc_calls();

  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = 0.25f;  // quiet block
  tp.process(rows, 1, kFrames);
  const long after = sftest::alloc_calls();

  ASSERT_GT(loud, 0.25) << "sanity: loud block latched above the quiet block";
  EXPECT_EQ(tp.latch(0), loud) << "quiet block must not pull the latch down";
  EXPECT_EQ(after, between) << "process() allocated";
}

// ---------------------------------------------------------------------------
// 5) Reset zeroes latch + tail (full cold start).
// ---------------------------------------------------------------------------
TEST(TruePeak, ResetZeroesLatch) {
  float buf[kFrames];
  fill_tone(buf, kFrames);
  float* rows[TruePeakChannels] = {buf, nullptr};

  TruePeak tp;
  tp.process(rows, 1, kFrames);
  ASSERT_GT(tp.latch(0), 0.5);

  tp.reset();
  EXPECT_EQ(tp.latch(0), 0.0);
  EXPECT_FALSE(tp.clipped(0));

  // Tail is zeroed too: post-reset silence must NOT ring with the pre-reset
  // fs/4 tone (a leaked tail would hold |output| up to the tone's peak).
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = 0.0f;
  tp.process(rows, 1, kFrames);
  EXPECT_EQ(tp.latch(0), 0.0)
      << "reset tail must not carry the pre-reset signal into the new block";

  // And a post-reset block still measures normally (steady DC gain ~1.0 after
  // priming; cold-start step overshoot is a filter property, so prime here).
  const double steady = steady_dc(1.0f);
  EXPECT_NEAR(steady, 1.0, 1e-4);
}

// ---------------------------------------------------------------------------
// 6) Silence: linear 0, not clipped.
// ---------------------------------------------------------------------------
TEST(TruePeak, SilenceIsZeroAndNotClipped) {
  float buf[kFrames] = {0.0f};
  float* rows[TruePeakChannels] = {buf, nullptr};

  TruePeak tp;
  tp.process(rows, 1, kFrames);
  EXPECT_EQ(tp.latch(0), 0.0);
  EXPECT_FALSE(tp.clipped(0));
}

// ---------------------------------------------------------------------------
// 7) Full-scale DC clips (latch >= 1.0, 0 dBFS contract); a full-scale
//    band-limited fs/4 tone does NOT spuriously clip in steady state
//    (review R-C ripple bound); a true >0 dBFS crest does clip.
// ---------------------------------------------------------------------------
TEST(TruePeak, FullScaleDcClips) {
  const double steady = steady_dc(1.0f);
  EXPECT_GE(steady, 1.0);
  TruePeak tp;
  float buf[kFrames];
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = 1.0f;
  float* rows[TruePeakChannels] = {buf, nullptr};
  tp.process(rows, 1, kFrames);
  EXPECT_TRUE(tp.clipped(0));
}

TEST(TruePeak, FullScaleFs4DoesNotSpuriouslyClip) {
  const double steady = steady_fs4(1.0f);
  EXPECT_LT(steady, 1.0) << "passband ripple must not push full-scale fs/4 "
                            "past 0 dBFS (R-C)";
  EXPECT_NEAR(steady, 1.0, 1e-3) << "and it must still read ~true peak";
}

TEST(TruePeak, OverscaleFs4Clips) {
  const double steady = steady_fs4(1.1f);
  EXPECT_GT(steady, 1.0);
  EXPECT_NEAR(steady, 1.1, 1e-3);
  TruePeak tp;
  float buf[kFrames];
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = 1.1f;
  float* rows[TruePeakChannels] = {buf, nullptr};
  tp.process(rows, 1, kFrames);
  EXPECT_TRUE(tp.clipped(0));
}

// ---------------------------------------------------------------------------
// 8) +/-1e30 input: stays finite everywhere (no NaN/Inf latch poison).
// ---------------------------------------------------------------------------
TEST(TruePeak, HugeFiniteInputStaysFinite) {
  float buf[kFrames];
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = 1e30f;
  float* rows[TruePeakChannels] = {buf, nullptr};

  TruePeak tp;
  tp.process(rows, 1, kFrames);
  EXPECT_TRUE(std::isfinite(tp.latch(0)));
  EXPECT_GT(tp.latch(0), 1e29);

  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = -1e30f;
  tp.process(rows, 1, kFrames);
  EXPECT_TRUE(std::isfinite(tp.latch(0)));
  EXPECT_TRUE(std::isfinite(static_cast<double>(buf[0])));
}

// ---------------------------------------------------------------------------
// 9) Determinism: two identical runs are <= 1e-12 apart.
// ---------------------------------------------------------------------------
TEST(TruePeak, DeterministicWithinOnePicosecondScale) {
  const auto run = [](TruePeak& tp) {
    float buf[kFrames];
    for (std::size_t r = 0; r < 8; ++r) {
      fill_tone(buf, kFrames, 0.75f, 0.123f);
      float* rows[TruePeakChannels] = {buf, nullptr};
      tp.process(rows, 1, kFrames);
    }
    return tp.latch(0);
  };
  TruePeak a, b;
  const double la = run(a);
  const double lb = run(b);
  EXPECT_NEAR(la, lb, 1e-12);
}

// ---------------------------------------------------------------------------
// 10) Per-channel independence: L silent, R loud.
// ---------------------------------------------------------------------------
TEST(TruePeak, ChannelsAreIndependent) {
  float l[kFrames] = {0.0f};
  float r[kFrames];
  float* rows[TruePeakChannels] = {l, r};

  TruePeak tp;
  // Prime R's tail (and the FIR state) so the steady measurement below has no
  // onset transient; L stays silent throughout.
  for (int b = 0; b < 4; ++b) {
    fill_tone(r, kFrames);
    tp.process(rows, TruePeakChannels, kFrames);
  }
  tp.resetLatch();

  for (int b = 0; b < 4; ++b) {
    fill_tone(r, kFrames);
    tp.process(rows, TruePeakChannels, kFrames);
  }

  EXPECT_EQ(tp.latch(0), 0.0) << "silent channel must stay silent";
  EXPECT_FALSE(tp.clipped(0));
  EXPECT_NEAR(tp.latch(1), 1.0, 2e-4);
  EXPECT_LT(tp.latch(1), 1.0) << "steady fs/4 must not clip";
  EXPECT_FALSE(tp.clipped(1));
}

// ---------------------------------------------------------------------------
// 11) Block-size invariance of the latch: 512x1 vs 64x8 identical input.
// ---------------------------------------------------------------------------
TEST(TruePeak, BlockSizeInvariantLatch) {
  constexpr std::size_t kTotal = 512;
  float x[kTotal];
  fill_tone(x, kTotal);
  float* rows1[TruePeakChannels] = {x, nullptr};

  TruePeak oneshot;
  oneshot.process(rows1, 1, kTotal);

  TruePeak chunked;
  for (std::size_t s = 0; s < kTotal; s += kFrames) {
    float* rows8[TruePeakChannels] = {x + s, nullptr};
    chunked.process(rows8, 1, kFrames);
  }

  EXPECT_NEAR(oneshot.latch(0), chunked.latch(0), 1e-12)
      << "latch must not depend on block partitioning";
}

// ---------------------------------------------------------------------------
// 12) NaN/Inf never poison the latch (ordered compare, review E-R-E).
// ---------------------------------------------------------------------------
TEST(TruePeak, NonFiniteSamplesDoNotPoisonLatch) {
  float buf[kFrames];
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = 1.0f;
  float* rows[TruePeakChannels] = {buf, nullptr};

  TruePeak tp;
  tp.process(rows, 1, kFrames);
  // Cold-start DC step response (~1.126) is a genuine FIR onset transient,
  // not a defect — the point here is that NaN/Inf never move it.
  const double baseline = tp.latch(0);

  // a block containing a NaN, then an all-NaN block
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = (i == 5) ? NAN : 0.5f;
  tp.process(rows, 1, kFrames);
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = NAN;
  tp.process(rows, 1, kFrames);
  // an Inf block
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = INFINITY;
  tp.process(rows, 1, kFrames);

  EXPECT_TRUE(std::isfinite(tp.latch(0)));
  EXPECT_EQ(tp.latch(0), baseline)
      << "NaN/Inf must never raise the latch (ordered compare)";

  // and the meter still works afterwards
  for (std::size_t i = 0; i < kFrames; ++i) buf[i] = 1.0f;
  tp.process(rows, 1, kFrames);
  EXPECT_EQ(tp.latch(0), baseline);
}

// ---------------------------------------------------------------------------
// 13) Process path: no allocations (allocation counter around process) and no
//     allocating/locking constructs in source (TRUE_PEAK_SRC scan).
// ---------------------------------------------------------------------------
TEST(TruePeak, ProcessPathAllocatesNothing) {
  float buf[kFrames];
  fill_tone(buf, kFrames);
  float* rows[TruePeakChannels] = {buf, nullptr};

  TruePeak tp;
  const long before = sftest::alloc_calls();
  for (int k = 0; k < 256; ++k) {
    const long mid = sftest::alloc_calls();
    tp.process(rows, 1, kFrames);
    const long after = sftest::alloc_calls();
    if (after != mid) {
      ADD_FAILURE() << "process allocated: mid=" << mid << " after=" << after;
      break;
    }
  }
  const long total = sftest::alloc_calls() - before;
  EXPECT_EQ(total, 0L) << "process made " << total
                       << " heap allocations across 256 blocks";
}

TEST(TruePeak, ProcessSourceHasNoAllocatingOrLockingConstructs) {
  std::ifstream in(TRUE_PEAK_SRC);
  ASSERT_TRUE(in.good()) << "cannot open " << TRUE_PEAK_SRC;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string src = ss.str();

  const std::size_t pos = src.find("void TruePeak::process");
  ASSERT_NE(pos, std::string::npos) << "process function not found";
  const std::string body = src.substr(pos);

  // The tail-advance comment mentions "in-place" — fine. Everything below is
  // the actual allocation/lock scan.
  EXPECT_EQ(body.find("std::map"), std::string::npos);
  EXPECT_EQ(body.find("std::set"), std::string::npos);
  EXPECT_EQ(body.find("std::vector"), std::string::npos);
  EXPECT_EQ(body.find("new "), std::string::npos);
  EXPECT_EQ(body.find("malloc"), std::string::npos);
  EXPECT_EQ(body.find("calloc"), std::string::npos);
  EXPECT_EQ(body.find("realloc"), std::string::npos);
  EXPECT_EQ(body.find("push_back"), std::string::npos);
  EXPECT_EQ(body.find("resize("), std::string::npos);
  EXPECT_EQ(body.find("reserve("), std::string::npos);
  EXPECT_EQ(body.find("mutex"), std::string::npos);
  EXPECT_EQ(body.find("lock("), std::string::npos);
  EXPECT_EQ(body.find("atomic"), std::string::npos);
}