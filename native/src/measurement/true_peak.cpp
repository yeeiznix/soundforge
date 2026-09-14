// SoundForge G4 P2 — true_peak.cpp
//
// Fixed 4-phase polyphase FIR interpolator + K-sample lookback tail +
// non-decaying latch (PLAN_G4 §4.3 D3, §6 P2). Compile-time constant
// coefficient table; the hot path (process) allocates nothing, takes no
// locks, and only touches caller buffers + the fixed per-channel state.
#include "true_peak.hpp"

#include <cmath>
#include <cstdint>

namespace sfmeasure {

// ---------------------------------------------------------------------------
// Coefficient table (generated, verified against the Python design workbench
// /tmp/opencode/tp_definitive.py — same values, bit-exact via hex literals).
//
// Prototype: Kaiser-windowed sinc lowpass designed at the 4x rate with the
// cutoff at input Nyquist (pi/4 of the 4x rate), M=24, beta=20, L=4M+1=97:
//
//     h[n] = sinc((n - (L-1)/2) / 4) * kaiser(n, L, beta)
//
// normalized so sum(h) = 4 (unity DC gain of the 4x-rate filter), decomposed
// into 4 phases g_p[m] = h[4m+p], each phase re-normalized to unit DC sum
// (per-phase DC unity — removes any systematic dB offset), then rounded to
// float32.
//
// Verification numbers (reference harness):
//   * phase lengths {25,24,24,24} -> K=24;
//   * fs/4 + pi/4 full-scale sine: sample peak 0.707106769, steady true peak
//     0.999999696 -> +3.01030 dB (tolerance [2.91, 3.11]);
//   * per-phase DC gain in [0.999999994, 1.000000044] -> |DC-1| <= 4.4e-8;
//   * steady full-scale in-band worst 0.999999641 < 1.0 -> a band-limited
//     full-scale tone below Nyquist does not spuriously clip (review R-C);
//   * block-size invariance of the latch and determinism <= 1e-12 confirmed.
// ---------------------------------------------------------------------------
static const float kPhaseTaps[TruePeakOversample][TruePeakTail + 1] = {
    // phase 0 (25 taps)
    {
        -0x1.14f7da0000000p-80f, 0x1.a1e9de0000000p-70f, -0x1.03cabe0000000p-67f, 0x1.0458500000000p-64f, -0x1.480ff60000000p-62f,
        0x1.2b17040000000p-60f, -0x1.a87bf20000000p-59f, 0x1.e9b3820000000p-58f, -0x1.d815400000000p-57f, 0x1.8351a20000000p-56f,
        -0x1.11c16e0000000p-55f, 0x1.4ff94c0000000p-55f, 0x1.0000000000000p+0f, 0x1.4ff94c0000000p-55f, -0x1.11c16e0000000p-55f,
        0x1.8351a20000000p-56f, -0x1.d815400000000p-57f, 0x1.e9b3820000000p-58f, -0x1.a87bf20000000p-59f, 0x1.2b17040000000p-60f,
        -0x1.480ff60000000p-62f, 0x1.0458500000000p-64f, -0x1.03cabe0000000p-67f, 0x1.a1e9de0000000p-70f, -0x1.14f7da0000000p-80f,
    },
    // phase 1 (24 taps)
    {
        -0x1.681a8c0000000p-28f, 0x1.fbcd3a0000000p-22f, -0x1.e6704a0000000p-18f, 0x1.da21a00000000p-15f, -0x1.3420660000000p-12f,
        0x1.2d56c20000000p-10f, -0x1.db23fc0000000p-9f, 0x1.3c860e0000000p-7f, -0x1.72b3500000000p-6f, 0x1.8f2ed80000000p-5f,
        -0x1.abb9160000000p-4f, 0x1.27d1280000000p-2f, 0x1.cb040e0000000p-1f, -0x1.4baada0000000p-3f, 0x1.2200bc0000000p-4f,
        -0x1.11e7aa0000000p-5f, 0x1.eb131e0000000p-7f, -0x1.8b10520000000p-8f, 0x1.11f9440000000p-9f, -0x1.3a275e0000000p-11f,
        0x1.19ac740000000p-13f, -0x1.6944f00000000p-16f, 0x1.168c8e0000000p-19f, -0x1.406bc80000000p-24f, 0x0.0p+0f,
    },
    // phase 2 (24 taps)
    {
        -0x1.1aa6960000000p-25f, 0x1.8554d20000000p-20f, -0x1.2da2940000000p-16f, 0x1.0555040000000p-13f, -0x1.39acaa0000000p-11f,
        0x1.2124540000000p-9f, -0x1.b366540000000p-8f, 0x1.17d2a80000000p-6f, -0x1.3f5e820000000p-5f, 0x1.54114c0000000p-4f,
        -0x1.74fd180000000p-3f, 0x1.407a5a0000000p-1f, 0x1.407a5a0000000p-1f, -0x1.74fd180000000p-3f, 0x1.54114c0000000p-4f,
        -0x1.3f5e820000000p-5f, 0x1.17d2a80000000p-6f, -0x1.b366540000000p-8f, 0x1.2124540000000p-9f, -0x1.39acaa0000000p-11f,
        0x1.0555040000000p-13f, -0x1.2da2940000000p-16f, 0x1.8554d20000000p-20f, -0x1.1aa6960000000p-25f, 0x0.0p+0f,
    },
    // phase 3 (24 taps)
    {
        -0x1.406bc80000000p-24f, 0x1.168c8e0000000p-19f, -0x1.6944f00000000p-16f, 0x1.19ac740000000p-13f, -0x1.3a275e0000000p-11f,
        0x1.11f9440000000p-9f, -0x1.8b10520000000p-8f, 0x1.eb131e0000000p-7f, -0x1.11e7aa0000000p-5f, 0x1.2200bc0000000p-4f,
        -0x1.4baada0000000p-3f, 0x1.cb040e0000000p-1f, 0x1.27d1280000000p-2f, -0x1.abb9160000000p-4f, 0x1.8f2ed80000000p-5f,
        -0x1.72b3500000000p-6f, 0x1.3c860e0000000p-7f, -0x1.db23fc0000000p-9f, 0x1.2d56c20000000p-10f, -0x1.3420660000000p-12f,
        0x1.da21a00000000p-15f, -0x1.e6704a0000000p-18f, 0x1.fbcd3a0000000p-22f, -0x1.681a8c0000000p-28f, 0x0.0p+0f,
    },
};

static const std::size_t kPhaseLen[TruePeakOversample] = {25, 24, 24, 24};

TruePeak::TruePeak() { reset(); }

void TruePeak::reset() {
  for (std::size_t c = 0; c < TruePeakChannels; ++c) {
    m_latch[c] = 0.0;
    for (std::size_t k = 0; k < TruePeakTail; ++k) m_tail[c][k] = 0.0f;
  }
}

void TruePeak::resetLatch() {
  for (std::size_t c = 0; c < TruePeakChannels; ++c) m_latch[c] = 0.0;
}

double TruePeak::latch(std::size_t ch) const {
  if (ch >= TruePeakChannels) return 0.0;
  return m_latch[ch];
}

void TruePeak::process(float* const* channels, std::size_t nch,
                       std::size_t frames) {
  if (channels == nullptr || frames == 0) return;
  const std::size_t nc =
      nch < TruePeakChannels ? nch : TruePeakChannels;  // engine contract: 2

  for (std::size_t c = 0; c < nc; ++c) {
    const float* x = channels[c];
    if (x == nullptr) continue;  // caller may leave a slot empty

    // 1) 4x polyphase FIR over (tail ++ block); track this block's |peak|.
    double peak = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
      const double xi = x[i];
      for (std::size_t p = 0; p < TruePeakOversample; ++p) {
        const float* g = kPhaseTaps[p];
        double acc = 0.0;
        for (std::size_t m = 0; m < kPhaseLen[p]; ++m) {
          double s;
          if (i >= m) {
            s = x[i - m];  // within this block
          } else {
            // m - i positions back, into the lookback tail:
            // tail index = K - (m - i)  (m > i here, so m - i is a valid
            // size_t; tail[K-1] is newest history)
            s = m_tail[c][TruePeakTail - (m - i)];
          }
          acc += static_cast<double>(g[m]) * s;
        }
        const double a = std::fabs(acc);
        // Ordered finite compare: NaN/Inf never reach `peak` -> a non-finite
        // sample cannot poison the latch (review E-R-E).
        if (std::isfinite(a) && a > peak) peak = a;
      }
    }

    // 2) Latch: non-decaying max since construction/reset.
    if (peak > m_latch[c]) m_latch[c] = peak;

    // 3) Slide the tail window: keep the last K samples of (old tail + block).
    if (frames >= TruePeakTail) {
      for (std::size_t k = 0; k < TruePeakTail; ++k)
        m_tail[c][k] = x[frames - TruePeakTail + k];
    } else {
      // updated_tail[j] = comb[frames + j]; comb[t] = tail[t] for t < K and
      // comb[K + t] = x[t]. Reads indices > j (frames >= 1), so the in-place
      // update never reads a slot already overwritten.
      for (std::size_t j = 0; j < TruePeakTail; ++j) {
        const std::size_t f = frames + j;
        m_tail[c][j] = (f < TruePeakTail) ? m_tail[c][f] : x[f - TruePeakTail];
      }
    }
  }
}

}  // namespace sfmeasure