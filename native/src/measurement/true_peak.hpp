// SoundForge G4 P2 — per-channel 4x true-peak meter (PLAN_G4 §4.3 D3, §6 P2).
//
// Leaf module `sfmeasure`:
//   * no SoundForge target dependency, no locks/malloc in the process path
//     (PLAN_G4 B-3);
//   * raw `float* const*` interleaved channels — no AudioBlock/sfdsp types;
//   * fixed polyphase FIR + per-channel K-sample lookback tail + non-decaying
//     max latch; 4x oversampling is the standard true-peak approximation
//     (ITU-R BS.1770 class; an *approximation*, not certified — G4-2).
#ifndef SFMEASURE_TRUE_PEAK_HPP
#define SFMEASURE_TRUE_PEAK_HPP

#include <cstddef>

namespace sfmeasure {

// Polyphase FIR shape: Kaiser-windowed sinc prototype, M=24, beta=20,
// L = 4*M+1 = 97 taps at the 4x rate (input Nyquist cutoff). The four phase
// branches have lengths {25, 24, 24, 24}; the lookback tail must cover the
// longest branch minus one sample (tap m=24 of the current sample reaches
// back 24 positions), hence K = 24.
static constexpr std::size_t TruePeakOversample = 4;
static constexpr std::size_t TruePeakTail = 24;   // max phase length - 1
static constexpr std::size_t TruePeakChannels = 2;  // engine contract is stereo

// Per-channel stateful true-peak meter. One object per measured bus.
//
// State: for each channel, a flat 4x-rate polyphase FIR (coefficient table,
// no per-call state) over a K-sample lookback tail, and a non-decaying
// |oversampled| max latch.
//
// Thread-safety: portable single-thread use (the engine drives the meter from
// one lane); the process path itself is lock-free/atomic-free by construction.
class TruePeak {
 public:
  // latch = 0, tails zeroed (cold-start state).
  TruePeak();

  // Process one block. `channels` points to `nch` row pointers into mutable
  // float buffers (read-only here); only the first TruePeakChannels rows are
  // consulted. Each row contributes `frames` consecutive samples.
  //
  // Runs the 4x interpolator over (tail ++ block) for every channel, then
  // monotone-updates each channel latch with the block's local |peak|.
  // NaN/Inf outputs never update the latch (ordered finite compare, review
  // E-R-E): a non-finite input sample cannot permanently poison the meter.
  //
  // No allocation, no locks, no syscalls.
  void process(float* const* channels, std::size_t nch, std::size_t frames);

  // Running true-peak magnitude of channel ch since construction / last
  // reset. 0 when silent. Out-of-range ch reads 0.
  double latch(std::size_t ch) const;

  // 0 dBFS contract: clipped <=> latch >= 1.0 (D3; NOT BS.1770's -1 dBTP —
  // G4-2).
  bool clipped(std::size_t ch) const { return latch(ch) >= 1.0; }

  // Full cold-start reset: zeroes the latches AND the tails.
  void reset();

  // `reset_meters` semantics (PLAN_G4 D3, P4): zeroes the latches only, keeps
  // the per-channel tails so a primed steady-state measurement resumes without
  // the FIR onset transient.
  void resetLatch();

 private:
  // tail[c][k] = input sample k positions before the newest, newest last:
  // tail[c][K-1] is the previous block's last sample.
  float m_tail[TruePeakChannels][TruePeakTail];
  double m_latch[TruePeakChannels];
};

}  // namespace sfmeasure

#endif  // SFMEASURE_TRUE_PEAK_HPP