// SoundForge G4 P1 — RenderPlan tests (PLAN_G4 §6 P1, §7.1
// test_render_plan.cpp row).
//
// Proves the dsp_render.cpp:27 seam is fulfilled:
//   * compile_render_plan + render_chain_planned reproduce the G3 render
//     semantics (the G3 cases themselves live unmodified in test_dsp_render.cpp);
//   * the execute path allocates NOTHING (allocation-counter hook + source scan),
//     which is the whole point of the preallocated pool.
#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "dsp_internal.hpp"
#include "graph_internal.hpp"
#include "render_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef RENDER_PLAN_SRC
#define RENDER_PLAN_SRC ""
#endif

namespace {

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

sfcore::SignalGraphDoc simple_chain(double src_db) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, src_db, 0.0));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("src", "out"));
  return g;
}

sfcore::dsp::AudioBlock dc_block(std::size_t n) {
  sfcore::dsp::AudioBlock b{};
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
// Compile-time plan shape
// ---------------------------------------------------------------------------

TEST(RenderPlan, CompileOrdersNodesTopologicallyAndResolvesTarget) {
  const sfcore::SignalGraphDoc g = simple_chain(-6.0);
  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;
  ASSERT_EQ(plan.nodes.size(), 2u);
  ASSERT_EQ(plan.preds.size(), 2u);
  ASSERT_GE(plan.out_index, 0);
  EXPECT_EQ(plan.nodes[static_cast<std::size_t>(plan.out_index)].kind,
            sfcore::SfNodeOutput);
  // preds is index-based: the output's predecessor is the source.
  EXPECT_EQ(plan.preds[static_cast<std::size_t>(plan.out_index)].size(), 1u);
}

TEST(RenderPlan, PrecomputedGainAndPanMatchG3Expression) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, -6.0, 0.0));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.75));
  g.edges.push_back(mk_edge("src", "out"));

  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;

  const float expect_gain = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  EXPECT_FLOAT_EQ(plan.nodes[0].gain, expect_gain);
  EXPECT_FLOAT_EQ(plan.nodes[1].pan, 0.75f);
  EXPECT_TRUE(plan.nodes[1].pan_active);
  EXPECT_FALSE(plan.nodes[0].pan_active);  // pan==0 -> passthrough
}

// ---------------------------------------------------------------------------
// Execute semantics (mirror the G3 test_dsp_render intents through the plan)
// ---------------------------------------------------------------------------

TEST(RenderPlan, ExecuteMatchesG3RenderChainBitForBit) {
  // src(-6 dB) -> proc(+3 dB, pan 0.5) -> out: planned execute vs the G3
  // render_chain facade must agree to 1e-12.
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, -6.0, 0.0));
  g.nodes.push_back(mk_node("proc", sfcore::SfNodeProcessor, 3.0, 0.5));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("src", "proc"));
  g.edges.push_back(mk_edge("proc", "out"));

  const std::size_t n = 256;
  sfcore::dsp::AudioBlock via_g3 = sine_block(n);
  std::string err;
  ASSERT_TRUE(sfcore::render_chain(g, via_g3, "out", err)) << err;

  sfcore::dsp::RenderPlan plan;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;
  sfcore::dsp::AudioBlock via_plan = sine_block(n);
  ASSERT_TRUE(sfcore::dsp::render_chain_planned(plan, via_plan, err)) << err;

  EXPECT_LE(max_abs_diff(via_g3, via_plan), 1e-12f);
}

TEST(RenderPlan, ExecuteMinusSixDbIsHalfAmplitude) {
  const sfcore::SignalGraphDoc g = simple_chain(-6.0);
  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;

  const std::size_t n = sfcore::dsp::kBlockMaxSamples;
  sfcore::dsp::AudioBlock b = dc_block(n);
  ASSERT_TRUE(sfcore::dsp::render_chain_planned(plan, b, err)) << err;
  const float half = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_FLOAT_EQ(b.L[i], half) << "i=" << i;
    EXPECT_FLOAT_EQ(b.R[i], half) << "i=" << i;
  }
}

TEST(RenderPlan, ExecuteRejectsBlockOverHardMaxAtEntry) {
  // SEC-G4-04 defense-in-depth bound (ORC-G4P4-02): render_chain_planned must
  // reject an oversized block itself, before any scratch write.
  const sfcore::SignalGraphDoc g = simple_chain(0.0);
  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;

  sfcore::dsp::AudioBlock b = dc_block(sfcore::dsp::kBlockMaxSamples);
  b.n = sfcore::dsp::kBlockMaxSamples + 1;  // simulate an out-of-contract block
  EXPECT_FALSE(sfcore::dsp::render_chain_planned(plan, b, err));
  EXPECT_EQ(err, "render_plan: block exceeds max");

  // The boundary itself still executes (hard max is valid).
  b.n = sfcore::dsp::kBlockMaxSamples;
  EXPECT_TRUE(sfcore::dsp::render_chain_planned(plan, b, err)) << err;
}

TEST(RenderPlan, ExecuteMuteAndSoloExclusion) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("s1", sfcore::SfNodeSource, -3.0, 0.0, false, true));
  g.nodes.push_back(mk_node("s2", sfcore::SfNodeSource, 0.0, 0.0));
  g.nodes.push_back(mk_node("proc", sfcore::SfNodeProcessor, 0.0, 0.0));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("s1", "proc"));
  g.edges.push_back(mk_edge("s2", "proc"));
  g.edges.push_back(mk_edge("proc", "out"));

  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;

  const std::size_t n = 256;
  const float g1 = static_cast<float>(std::pow(10.0, -3.0 / 20.0));
  sfcore::dsp::AudioBlock iso = dc_block(n);
  ASSERT_TRUE(sfcore::dsp::render_chain_planned(plan, iso, err)) << err;
  for (std::size_t i = 0; i < n; ++i) EXPECT_FLOAT_EQ(iso.L[i], g1) << "i=" << i;

  // Clear solo (recompile) -> both sources mix coherently.
  g.nodes[0].mixer.solo = false;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;
  sfcore::dsp::AudioBlock mix = dc_block(n);
  ASSERT_TRUE(sfcore::dsp::render_chain_planned(plan, mix, err)) << err;
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_FLOAT_EQ(mix.L[i], 1.0f + g1) << "i=" << i;
  }
}

TEST(RenderPlan, ExecuteEmptyGraphIsIdentity) {
  sfcore::SignalGraphDoc g;
  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "no-such-output", plan, err))
      << err;
  const std::size_t n = 128;
  sfcore::dsp::AudioBlock in = sine_block(n);
  sfcore::dsp::AudioBlock out = in;
  ASSERT_TRUE(sfcore::dsp::render_chain_planned(plan, out, err)) << err;
  EXPECT_LE(max_abs_diff(out, in), 1e-12f);
}

TEST(RenderPlan, ExecuteUnknownTargetGivesSilenceAndFalse) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, 0.0, 0.0));
  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "ghost", plan, err)) << err;
  EXPECT_EQ(plan.out_index, -1);

  const std::size_t n = 64;
  sfcore::dsp::AudioBlock b = dc_block(n);
  EXPECT_FALSE(sfcore::dsp::render_chain_planned(plan, b, err));
  EXPECT_FALSE(err.empty());
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_FLOAT_EQ(b.L[i], 0.0f) << "i=" << i;
    EXPECT_FLOAT_EQ(b.R[i], 0.0f) << "i=" << i;
  }
}

TEST(RenderPlan, ExecuteDefaultPanIsPassthrough) {
  const sfcore::SignalGraphDoc g = simple_chain(0.0);
  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;
  const std::size_t n = 64;
  sfcore::dsp::AudioBlock in = sine_block(n);
  sfcore::dsp::AudioBlock out = in;
  ASSERT_TRUE(sfcore::dsp::render_chain_planned(plan, out, err)) << err;
  EXPECT_LE(max_abs_diff(out, in), 1e-12f);
}

// ---------------------------------------------------------------------------
// THE seam assertion: zero allocation on the execute path
// ---------------------------------------------------------------------------

TEST(RenderPlan, ExecutePathAllocatesNothing) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, -6.0, 0.0));
  g.nodes.push_back(mk_node("proc", sfcore::SfNodeProcessor, 3.0, 0.5));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0, 0.0));
  g.edges.push_back(mk_edge("src", "proc"));
  g.edges.push_back(mk_edge("proc", "out"));

  sfcore::dsp::RenderPlan plan;
  std::string err;
  ASSERT_TRUE(sfcore::dsp::compile_render_plan(g, "out", plan, err)) << err;

  // Warm up once (no lazy allocations may hide in the first call), then count.
  sfcore::dsp::AudioBlock warm = sine_block(512);
  ASSERT_TRUE(sfcore::dsp::render_chain_planned(plan, warm, err)) << err;

  const long before = sftest::alloc_calls();
  for (int k = 0; k < 64; ++k) {
    sfcore::dsp::AudioBlock b = sine_block(512);  // allocated BEFORE the region
    // (sine_block itself allocates nothing; AudioBlock is stack/POD)
    const long mid = sftest::alloc_calls();
    const bool ok = sfcore::dsp::render_chain_planned(plan, b, err);
    const long after = sftest::alloc_calls();
    if (!ok || after != mid) {
      ADD_FAILURE() << "execute allocated: mid=" << mid << " after=" << after;
      break;
    }
  }
  const long total = sftest::alloc_calls() - before;
  EXPECT_EQ(total, 0L) << "render_chain_planned made " << total
                       << " heap allocations across 64 executions";
}

TEST(RenderPlan, ExecuteSourceScanHasNoAllocatingCalls) {
  // Belt-and-suspenders: the allocation counter proves THIS binary; the source
  // scan proves the execute function itself contains no allocating construct
  // (std::map/std::set/new/vector growth).
  std::ifstream in(RENDER_PLAN_SRC);
  ASSERT_TRUE(in.good()) << "cannot open " << RENDER_PLAN_SRC;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string src = ss.str();

  const std::size_t pos = src.find("bool render_chain_planned");
  ASSERT_NE(pos, std::string::npos) << "execute function not found";
  const std::string body = src.substr(pos);
  EXPECT_EQ(body.find("std::map"), std::string::npos);
  EXPECT_EQ(body.find("std::set"), std::string::npos);
  EXPECT_EQ(body.find("new "), std::string::npos);
  EXPECT_EQ(body.find("push_back"), std::string::npos);
  EXPECT_EQ(body.find("resize("), std::string::npos);
  EXPECT_EQ(body.find("reserve("), std::string::npos);
}
