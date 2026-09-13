// SoundForge G3 P3 — chain-render harness tests (PLAN_G3 §4.2 D2 / §6 P3 /
// §7.1 "Chain-render harness" row).
//
// The harness under test is the LIBRARY function sfcore::render_chain
// (native/src/graph/dsp_render.cpp, compiled INTO sfgraph) — NOT a test-local
// copy. Graphs are built directly on the internal SignalGraphDoc shape
// (graph_internal.hpp); the ABI -> doc path and evaluate_mixer parity are
// covered by test_graph_mixer.cpp / test_graph_nodes.cpp.
//
// Node semantics (mirroring evaluate_mixer, routing.cpp):
//   - the input block is fed to EVERY source node;
//   - a node's output = gate * gain * pan( SUM over in-edges of the
//     predecessors' outputs ); mute/solo exclusion zeroes the node (silence
//     propagates downstream);
//   - pan == 0.0 (the default) means "no pan operation": render_chain calls
//     apply_pan only for pan != 0 nodes — see dsp_internal.hpp apply_pan
//     (kernel law vs harness convention coexist by design).
//
// Kernels are stateless and sample-wise, so block segmentation cannot change
// the output (asserted by the 1024-sample split test, §6 P3 acceptance).
#include <gtest/gtest.h>

#include "graph_internal.hpp"
#include "dsp_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Graph builders — direct SignalGraphDoc construction (no ABI, no casts).
// ---------------------------------------------------------------------------
sfcore::SignalNode mk_node(const std::string& id, int kind, double gain_db,
                           double pan, bool mute = false, bool solo = false) {
  sfcore::SignalNode n;
  n.id = id;
  n.kind = kind;
  n.name = id;
  n.mixer.gainDb = gain_db;
  n.mixer.pan = pan;
  n.mixer.mute = mute;
  n.mixer.solo = solo;
  return n;
}

sfcore::SignalEdge mk_edge(const std::string& from, const std::string& to) {
  sfcore::SignalEdge e;
  e.fromNodeId = from;
  e.toNodeId = to;
  return e;
}

// src(gain_db) -> out chain with default pan, plus the chain src -> proc ->
// out variant used by several tests.
sfcore::SignalGraphDoc simple_chain(double src_db) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, src_db, 0.0));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("src", "out"));
  return g;
}

// ---------------------------------------------------------------------------
// Stereo block generators (AudioBlock — the D2 caller-owned shape). Mono
// sources are duplicated into both channels.
// ---------------------------------------------------------------------------
sfcore::dsp::AudioBlock dc_block(std::size_t n) {
  sfcore::dsp::AudioBlock b{};  // value-init: arrays are zeroed
  b.n = n;
  for (std::size_t i = 0; i < n; ++i) {
    b.L[i] = 1.0f;
    b.R[i] = 1.0f;
  }
  return b;
}

sfcore::dsp::AudioBlock sine_block(std::size_t n) {
  sfcore::dsp::AudioBlock b{};
  b.n = n;
  for (std::size_t i = 0; i < n; ++i) {
    const double v =
        std::sin(2.0 * sfcore::dsp::kPi * static_cast<double>(i) / 64.0);
    b.L[i] = static_cast<float>(v);
    b.R[i] = static_cast<float>(v);
  }
  return b;
}

// Slice samples [off, off+n) out of two (stereo) stream vectors.
sfcore::dsp::AudioBlock slice(const std::vector<float>& L,
                              const std::vector<float>& R, std::size_t off,
                              std::size_t n) {
  sfcore::dsp::AudioBlock b{};
  b.n = n;
  for (std::size_t i = 0; i < n; ++i) {
    b.L[i] = L[off + i];
    b.R[i] = R[off + i];
  }
  return b;
}

// Single-call harness wrapper: renders `in` through `g` up to `out_id`.
sfcore::dsp::AudioBlock render(const sfcore::SignalGraphDoc& g,
                               sfcore::dsp::AudioBlock in,
                               const std::string& out_id) {
  std::string err;
  EXPECT_TRUE(sfcore::render_chain(g, in, out_id, err)) << err;
  return in;
}

float max_abs_diff(const sfcore::dsp::AudioBlock& a,
                   const sfcore::dsp::AudioBlock& b) {
  float d = 0.0f;
  const std::size_t n = std::min(a.n, b.n);
  for (std::size_t i = 0; i < n; ++i) {
    d = std::max(d, std::fabs(a.L[i] - b.L[i]));
    d = std::max(d, std::fabs(a.R[i] - b.R[i]));
  }
  return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// Tests (7 intents from PLAN_G3 §6 P3 acceptance + the pan==0 passthrough
// pin, ORC-P3-2(b))
// ---------------------------------------------------------------------------

TEST(Render, TwoNodeChainMinusSixDbIsHalfAmplitude) {
  // src(-6 dB) -> out: a DC block renders to exactly half amplitude
  // (10^(-6/20), float-exact on the same computation path). The default-pan
  // chain must NOT be attenuated by the equal-power center law — pan==0
  // nodes are pass-through (ORC-P3-2).
  const sfcore::SignalGraphDoc g = simple_chain(-6.0);

  const std::size_t n = sfcore::dsp::kBlockMaxSamples;
  const sfcore::dsp::AudioBlock out = render(g, dc_block(n), "out");
  const float half = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  ASSERT_EQ(out.n, n);
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_FLOAT_EQ(out.L[i], half) << "i=" << i;
    EXPECT_FLOAT_EQ(out.R[i], half) << "i=" << i;
  }
}

TEST(Render, MutedProcessorContributesSilence) {
  // src -> proc(muted) -> out: the muted node's silence propagates through
  // the whole chain (mix_bus accumulates nothing).
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, 0.0, 0.0));
  g.nodes.push_back(mk_node("proc", sfcore::SfNodeProcessor, 0.0, 0.0, /*mute=*/true));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("src", "proc"));
  g.edges.push_back(mk_edge("proc", "out"));

  const std::size_t n = 128;
  const sfcore::dsp::AudioBlock out = render(g, sine_block(n), "out");
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_FLOAT_EQ(out.L[i], 0.0f) << "i=" << i;
    EXPECT_FLOAT_EQ(out.R[i], 0.0f) << "i=" << i;
  }
}

TEST(Render, SoloIsolatesSourceThroughSharedChain) {
  // s1, s2 -> proc -> out. Soloing s1 keeps only s1's branch audible (the
  // G2 solo semantics: upstream walk starts from soloed nodes only).
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("s1", sfcore::SfNodeSource, -3.0, 0.0, /*mute=*/false,
                            /*solo=*/true));
  g.nodes.push_back(mk_node("s2", sfcore::SfNodeSource, 0.0, 0.0));
  g.nodes.push_back(mk_node("proc", sfcore::SfNodeProcessor, 0.0, 0.0));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("s1", "proc"));
  g.edges.push_back(mk_edge("s2", "proc"));
  g.edges.push_back(mk_edge("proc", "out"));

  const std::size_t n = 256;
  const float g1 = static_cast<float>(std::pow(10.0, -3.0 / 20.0));

  // Solo active: output is exactly s1's -3 dB signal; s2's sibling branch
  // stays silent.
  const sfcore::dsp::AudioBlock iso = render(g, dc_block(n), "out");
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_FLOAT_EQ(iso.L[i], g1) << "i=" << i;
    EXPECT_FLOAT_EQ(iso.R[i], g1) << "i=" << i;
  }

  // Control: clearing solo mixes both sources coherently (g1 + g2, same
  // accumulator order -> float-exact vs the explicit sum).
  g.nodes[0].mixer.solo = false;
  const sfcore::dsp::AudioBlock mix = render(g, dc_block(n), "out");
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_FLOAT_EQ(mix.L[i], 1.0f + g1) << "i=" << i;
    EXPECT_FLOAT_EQ(mix.R[i], 1.0f + g1) << "i=" << i;
  }
}

TEST(Render, DefaultPanNodeIsPassthrough) {
  // ORC-P3-2(b): harness convention pinned — a default-pan unity-gain node
  // passes the block through EXACTLY (bit-identical), NOT attenuated by the
  // equal-power center law (apply_pan(0) is only ever called with pan != 0
  // at render level; the kernel law is pinned separately).
  const sfcore::SignalGraphDoc g = simple_chain(0.0);

  const std::size_t n = 64;
  const sfcore::dsp::AudioBlock in = sine_block(n);
  const sfcore::dsp::AudioBlock out = render(g, in, "out");
  EXPECT_EQ(out.n, in.n);
  EXPECT_LE(max_abs_diff(out, in), 1e-12f);
}

TEST(Render, PanStereoSumEqualsMonoGain) {
  // src(0 dB) -> out(pan 0.75): equal-power panning preserves energy —
  // L^2 + R^2 == input^2 == (mono gain · input)^2 at every sample. Panned
  // right of center: R dominates L.
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, 0.0, 0.0));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.75));
  g.edges.push_back(mk_edge("src", "out"));

  const std::size_t n = 64;
  const sfcore::dsp::AudioBlock out = render(g, dc_block(n), "out");
  for (std::size_t i = 0; i < n; ++i) {
    const double l = out.L[i];
    const double r = out.R[i];
    EXPECT_NEAR(l * l + r * r, 1.0, 1e-5) << "i=" << i;  // == mono gain^2
    EXPECT_GT(l, 0.0) << "i=" << i;
    EXPECT_GT(r, 0.0) << "i=" << i;
  }
  EXPECT_LT(out.L[0], out.R[0]);
}

TEST(Render, Segmentation1024Deterministic) {
  // 1024-sample stream through src(-6 dB) -> proc(+3 dB, pan 0.5) -> out:
  // the plan's canonical split is 2×512 blocks (kBlockMaxSamples = 512); the
  // same stream split finer (4×256) must give identical sample-by-sample
  // output — stateless kernels, no inter-segment state bleed (§6 P3).
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, -6.0, 0.0));
  g.nodes.push_back(mk_node("proc", sfcore::SfNodeProcessor, 3.0, 0.5));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("src", "proc"));
  g.edges.push_back(mk_edge("proc", "out"));

  const std::size_t total = 2 * sfcore::dsp::kBlockMaxSamples;  // 1024
  std::vector<float> L(total), R(total);
  for (std::size_t i = 0; i < total; ++i) {
    const double v =
        std::sin(2.0 * sfcore::dsp::kPi * static_cast<double>(i) / 64.0);
    L[i] = static_cast<float>(v);
    R[i] = static_cast<float>(v);
  }

  // Plan split: 2 x 512 blocks.
  const sfcore::dsp::AudioBlock a0 = render(g, slice(L, R, 0, 512), "out");
  const sfcore::dsp::AudioBlock a1 = render(g, slice(L, R, 512, 512), "out");

  // Double-run determinism (same platform, 1e-12).
  EXPECT_LE(max_abs_diff(a0, render(g, slice(L, R, 0, 512), "out")), 1e-12f);
  EXPECT_LE(max_abs_diff(a1, render(g, slice(L, R, 512, 512), "out")), 1e-12f);

  // Finer split: 4 x 256 blocks — per segment, 2×256 must equal 1×512
  // («same output as one 512 block per segment»): no state crosses the
  // inter-segment boundary.
  const sfcore::dsp::AudioBlock c0 = render(g, slice(L, R, 0, 256), "out");
  const sfcore::dsp::AudioBlock c1 = render(g, slice(L, R, 256, 256), "out");
  const sfcore::dsp::AudioBlock c2 = render(g, slice(L, R, 512, 256), "out");
  const sfcore::dsp::AudioBlock c3 = render(g, slice(L, R, 768, 256), "out");

  sfcore::dsp::AudioBlock joined0{};  // segment 0 = c0 ++ c1
  joined0.n = 512;
  for (std::size_t i = 0; i < 256; ++i) {
    joined0.L[i] = c0.L[i];
    joined0.R[i] = c0.R[i];
    joined0.L[256 + i] = c1.L[i];
    joined0.R[256 + i] = c1.R[i];
  }
  EXPECT_LE(max_abs_diff(a0, joined0), 1e-12f);

  sfcore::dsp::AudioBlock joined1{};  // segment 1 = c2 ++ c3
  joined1.n = 512;
  for (std::size_t i = 0; i < 256; ++i) {
    joined1.L[i] = c2.L[i];
    joined1.R[i] = c2.R[i];
    joined1.L[256 + i] = c3.L[i];
    joined1.R[256 + i] = c3.R[i];
  }
  EXPECT_LE(max_abs_diff(a1, joined1), 1e-12f);
}

TEST(Render, EmptyGraphIsIdentity) {
  // No nodes: nothing to process — the block passes through unchanged.
  sfcore::SignalGraphDoc g;
  const std::size_t n = 128;
  const sfcore::dsp::AudioBlock in = sine_block(n);
  const sfcore::dsp::AudioBlock out = render(g, in, "no-such-output");
  EXPECT_EQ(out.n, in.n);
  EXPECT_LE(max_abs_diff(out, in), 1e-12f);
}

TEST(Render, ChainGainsMultiplyLikePathProduct) {
  // src(-6 dB) -> proc(+6 dB) -> out: per-node kernel gains multiply to the
  // desk path product 10^((-6+6)/20) = 1.0 — the DC block is unchanged
  // (within float multiply rounding).
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, -6.0, 0.0));
  g.nodes.push_back(mk_node("proc", sfcore::SfNodeProcessor, 6.0, 0.0));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("src", "proc"));
  g.edges.push_back(mk_edge("proc", "out"));

  const std::size_t n = 64;
  const sfcore::dsp::AudioBlock in = dc_block(n);
  const sfcore::dsp::AudioBlock out = render(g, in, "out");
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_NEAR(out.L[i], 1.0f, 1e-6f) << "i=" << i;
    EXPECT_NEAR(out.R[i], 1.0f, 1e-6f) << "i=" << i;
  }
}