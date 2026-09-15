// SoundForge G4 P4 — sf_audio_engine tests (PLAN_G4 §6 P4, §7.1
// test_audio_engine.cpp row; §4.2 D2, §4.3 D3 + SEC-G4-01/02, §4.5 D5, §4.6 D6).
//
// Covers: one-shot lifecycle + the real single-engine claim (R-D); configure/
// set_output gating; deterministic tick (known graph -> known scale + meter);
// the SEC-G4-05 ordered start gate; the PACE-vs-tick guard; pacer progress +
// bounded reap; SEC-G4-02 concurrent meter reads against a ticking pacer;
// SEC-G4-01 meter buffer contract (NOMEM + NUL-terminated, never partial);
// planValid on valid / unknown-target / cyclic graphs; last_report byte-identity
// passthrough (and that meter data never leaks into it).
#include <gtest/gtest.h>

#include "dsp_internal.hpp"
#include "graph_internal.hpp"
#include "sf_internal.hpp"
#include "soundforge/sf_audio_engine.h"
#include "soundforge/sf_command_queue.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_queue_runner.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;

sfcore::SignalNode mk_node(const std::string& id, int kind, double gain_db) {
  sfcore::SignalNode n;
  n.id = id;
  n.kind = kind;
  n.name = id;
  n.mixer.gainDb = gain_db;
  return n;
}

sfcore::SignalEdge mk_edge(const std::string& from, const std::string& to) {
  sfcore::SignalEdge e;
  e.fromNodeId = from;
  e.toNodeId = to;
  return e;
}

// src(gain_db) -> out, built directly on the internal doc shape.
void set_chain(sf_project_t* p, double src_db) {
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  ip->doc.signalGraph.nodes.clear();
  ip->doc.signalGraph.edges.clear();
  ip->doc.signalGraph.nodes.push_back(mk_node("src", sfcore::SfNodeSource, src_db));
  ip->doc.signalGraph.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0));
  ip->doc.signalGraph.edges.push_back(mk_edge("src", "out"));
}

// A cyclic graph must be injected directly: add_edge_impl rejects cycles.
void set_cycle(sf_project_t* p) {
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  ip->doc.signalGraph.nodes.clear();
  ip->doc.signalGraph.edges.clear();
  ip->doc.signalGraph.nodes.push_back(mk_node("a", sfcore::SfNodeProcessor, 0.0));
  ip->doc.signalGraph.nodes.push_back(mk_node("b", sfcore::SfNodeProcessor, 0.0));
  ip->doc.signalGraph.edges.push_back(mk_edge("a", "b"));
  ip->doc.signalGraph.edges.push_back(mk_edge("b", "a"));
}

template <typename F>
bool wait_until(F&& cond, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return cond();
}

// Host device callbacks used by the tick/pacer tests.
struct IoState {
  float dc = 0.0f;
  bool read_fail = false;
  std::atomic<int> reads{0};
  std::atomic<int> writes{0};
  std::atomic<int> write_fail{0};
  float last_l[sfcore::dsp::kBlockMaxSamples] = {};
  float last_r[sfcore::dsp::kBlockMaxSamples] = {};
};

sf_result_t capture_read(void* user, float* const* in, int32_t channels,
                         int32_t frames) {
  auto* s = static_cast<IoState*>(user);
  s->reads.fetch_add(1, std::memory_order_relaxed);
  if (s->read_fail) return SF_E_IO;
  for (int32_t c = 0; c < channels; ++c) {
    for (int32_t i = 0; i < frames; ++i) in[c][i] = s->dc;
  }
  return SF_OK;
}

sf_result_t capture_write(void* user, const float* const* out, int32_t channels,
                          int32_t frames) {
  auto* s = static_cast<IoState*>(user);
  s->writes.fetch_add(1, std::memory_order_relaxed);
  for (int32_t i = 0; i < frames; ++i) {
    s->last_l[i] = out[0][i];
    if (channels > 1) s->last_r[i] = out[1][i];
  }
  if (s->write_fail.load() > 0) return SF_E_IO;
  return SF_OK;
}

struct Fixture {
  sf_project_t* p = nullptr;
  sf_cmd_queue_t* q = nullptr;
  sf_audio_engine_t* e = nullptr;
  IoState io;

  bool make() {
    p = sf_project_create("E", nullptr);
    if (!p) return false;
    if (sf_cmd_queue_create(&q) != SF_OK) return false;
    return sf_audio_engine_create(&e, q, p) == SF_OK;
  }

  void configure_default() {
    sf_audio_engine_config_t cfg{};
    cfg.sample_rate = 48000;
    cfg.channels = 2;
    cfg.max_block_frames = 512;
    cfg.io.user = &io;
    cfg.io.read = &capture_read;
    cfg.io.write = &capture_write;
    ASSERT_EQ(sf_audio_engine_configure(e, &cfg), SF_OK);
  }

  ~Fixture() {
    if (e) {
      sf_audio_engine_stop(e);
      sf_audio_engine_join(e);
      sf_audio_engine_destroy(e);
    }
    if (q) sf_cmd_queue_destroy(q);
    if (p) sf_project_destroy(p);
  }
};

json meter(const sf_audio_engine_t* e) {
  char buf[1024];
  const sf_result_t rc = sf_audio_engine_meter_json(e, buf, sizeof(buf));
  EXPECT_EQ(rc, SF_OK);
  return json::parse(buf);
}

}  // namespace

// ---------------------------------------------------------------------------
// Argument validation + create guard
// ---------------------------------------------------------------------------

TEST(AudioEngine, CreateNullArguments) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_project_t* p = sf_project_create("E", nullptr);
  ASSERT_NE(p, nullptr);

  sf_audio_engine_t* e = nullptr;
  EXPECT_EQ(sf_audio_engine_create(nullptr, q, p), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_create(&e, nullptr, p), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_create(&e, q, nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(e, nullptr);

  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(AudioEngine, CreateClaimsSingleEngineSlot) {
  sf_project_t* p = sf_project_create("E", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);

  sf_audio_engine_t* e1 = nullptr;
  ASSERT_EQ(sf_audio_engine_create(&e1, q, p), SF_OK);
  EXPECT_NE(e1, nullptr);

  sf_audio_engine_t* e2 = reinterpret_cast<sf_audio_engine_t*>(0x1);
  EXPECT_EQ(sf_audio_engine_create(&e2, q, p), SF_E_IO);
  EXPECT_EQ(e2, nullptr);  // *out cleared on the rejected path
  EXPECT_STREQ(sf_last_error(p), "audio engine: create: engine already attached");

  ASSERT_EQ(sf_audio_engine_destroy(e1), SF_OK);
  // The claim is released: a fresh engine can attach after destroy.
  sf_audio_engine_t* e3 = nullptr;
  EXPECT_EQ(sf_audio_engine_create(&e3, q, p), SF_OK);
  ASSERT_EQ(sf_audio_engine_destroy(e3), SF_OK);

  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(AudioEngine, DestroyNullIsSafeNoop) {
  EXPECT_EQ(sf_audio_engine_destroy(nullptr), SF_OK);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(AudioEngine, LifecycleCreateConfigureSetOutputStartTickStopJoinDestroy) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());

  set_chain(f.p, -6.0);
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);

  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);
  EXPECT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);

  EXPECT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  EXPECT_EQ(sf_audio_engine_join(f.e), SF_OK);
  // Idempotent.
  EXPECT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  EXPECT_EQ(sf_audio_engine_join(f.e), SF_OK);
}

TEST(AudioEngine, TickBeforeStartRejected) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());

  EXPECT_EQ(sf_audio_engine_tick(f.e, 64), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), "audio engine: tick: engine not running");
}

TEST(AudioEngine, DoubleStartRejected) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());

  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);
  EXPECT_EQ(sf_audio_engine_start(f.e, 0), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), "audio engine: already started");
}

TEST(AudioEngine, ConfigureAndSetOutputRejectAfterStart) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  sf_audio_engine_config_t cfg{};
  cfg.sample_rate = 48000;
  cfg.channels = 2;
  cfg.max_block_frames = 128;
  EXPECT_EQ(sf_audio_engine_configure(f.e, &cfg), SF_E_IO);
  EXPECT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_E_IO);
}

TEST(AudioEngine, ConfigureValidation) {
  Fixture f;
  ASSERT_TRUE(f.make());

  sf_audio_engine_config_t cfg{};
  cfg.sample_rate = 0;
  cfg.channels = 2;
  cfg.max_block_frames = 512;
  EXPECT_EQ(sf_audio_engine_configure(f.e, &cfg), SF_E_INVALID_ARG);

  cfg.sample_rate = 48000;
  cfg.channels = 1;
  EXPECT_EQ(sf_audio_engine_configure(f.e, &cfg), SF_E_INVALID_ARG);

  cfg.channels = 2;
  cfg.max_block_frames = 0;
  EXPECT_EQ(sf_audio_engine_configure(f.e, &cfg), SF_E_INVALID_ARG);

  cfg.max_block_frames = 513;  // > kBlockMaxSamples
  EXPECT_EQ(sf_audio_engine_configure(f.e, &cfg), SF_E_INVALID_ARG);

  EXPECT_EQ(sf_audio_engine_configure(f.e, nullptr), SF_E_INVALID_ARG);
}

TEST(AudioEngine, TickFrameBoundsBothEdges) {
  Fixture f;
  ASSERT_TRUE(f.make());

  sf_audio_engine_config_t cfg{};
  cfg.sample_rate = 48000;
  cfg.channels = 2;
  cfg.max_block_frames = 64;
  cfg.io.user = &f.io;
  cfg.io.read = &capture_read;
  cfg.io.write = &capture_write;
  ASSERT_EQ(sf_audio_engine_configure(f.e, &cfg), SF_OK);
  set_chain(f.p, 0.0);
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  EXPECT_EQ(sf_audio_engine_tick(f.e, 0), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_tick(f.e, 65), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_tick(f.e, 1), SF_OK);   // lower edge
  EXPECT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);  // upper edge == max
}

TEST(AudioEngine, TickOverHardBlockMaxRejected) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());  // max_block_frames = 512
  set_chain(f.p, 0.0);
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  EXPECT_EQ(sf_audio_engine_tick(f.e, 513), SF_E_INVALID_ARG);
  // Frame-count rejection uses the thread-local error slot (same pattern as
  // the tick-bounds assertions: set_last_error, not the project handle).
  EXPECT_STREQ(sf_last_error(nullptr), "audio engine: tick: invalid frame count");
  EXPECT_EQ(sf_audio_engine_tick(f.e, 512), SF_OK);  // hard max still valid
}

TEST(AudioEngine, DestroyRejectsWhileRunningThenSucceedsAfterJoin) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  EXPECT_EQ(sf_audio_engine_destroy(f.e), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), "audio engine: destroy: engine active");

  ASSERT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_join(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_destroy(f.e), SF_OK);
  f.e = nullptr;  // freed
}

TEST(AudioEngine, OrderingGuardsRejectNullHandles) {
  EXPECT_EQ(sf_audio_engine_configure(nullptr, nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_set_output(nullptr, "x"), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_start(nullptr, 0), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_stop(nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_join(nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_tick(nullptr, 1), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_reset_meters(nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_meter_json(nullptr, nullptr, 0), SF_E_INVALID_ARG);
}

// SEC-G4-05: a standalone runner created in the create->start window must make
// engine start reject (the ordered gate reads runnerState BEFORE publishing).
TEST(AudioEngine, StartRejectsWhenStandaloneRunnerAlive) {
  Fixture f;
  ASSERT_TRUE(f.make());

  sf_queue_runner_t* standalone = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&standalone, f.q, f.p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(standalone), SF_OK);

  EXPECT_EQ(sf_audio_engine_start(f.e, 0), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), "audio engine: start: project not idle");

  // The standalone runner still owns the project: engine destroy is fine
  // (engine state is CREATED), and the standalone runner tears down next.
  sf_queue_runner_stop(standalone);
  sf_queue_runner_join(standalone);
  ASSERT_EQ(sf_queue_runner_destroy(standalone), SF_OK);
  ASSERT_EQ(sf_audio_engine_destroy(f.e), SF_OK);
  f.e = nullptr;
}

// Mirror of the start gate: create() itself must reject while a standalone
// runner holds the project (runnerState != Idle).
TEST(AudioEngine, CreateRejectsWhenStandaloneRunnerRunning) {
  sf_project_t* p = sf_project_create("E", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);

  sf_queue_runner_t* standalone = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&standalone, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(standalone), SF_OK);

  sf_audio_engine_t* e = reinterpret_cast<sf_audio_engine_t*>(0x1);
  EXPECT_EQ(sf_audio_engine_create(&e, q, p), SF_E_IO);
  EXPECT_EQ(e, nullptr);  // *out cleared on the rejected path
  EXPECT_STREQ(sf_last_error(p), "audio engine: create: project not idle");

  sf_queue_runner_stop(standalone);
  sf_queue_runner_join(standalone);
  ASSERT_EQ(sf_queue_runner_destroy(standalone), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// Deterministic tick: known graph -> known scale + meter
// ---------------------------------------------------------------------------

TEST(AudioEngine, TickRendersKnownScaleAndMeasuresIt) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());

  set_chain(f.p, -6.0);  // 10^(-6/20) = 0.501187
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  // Prime the meter tail, then zero the latches (tails kept) and measure.
  for (int i = 0; i < 8; ++i) ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);

  const float expect = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  EXPECT_NEAR(f.io.last_l[0], expect, 1e-6f);
  EXPECT_NEAR(f.io.last_r[0], expect, 1e-6f);

  const json m = meter(f.e);
  EXPECT_EQ(m["channels"], 2);
  EXPECT_EQ(m["oversample"], 4);
  EXPECT_EQ(m["truePeakLinear"].size(), 2u);
  EXPECT_NEAR(m["truePeakLinear"][0].get<double>(), expect, 1e-3);
  EXPECT_NEAR(m["truePeakLinear"][1].get<double>(), expect, 1e-3);
  EXPECT_NEAR(m["truePeakDb"][0].get<double>(), -6.0206, 0.05);
  EXPECT_EQ(m["clipped"][0], false);
  EXPECT_EQ(m["planValid"], true);
}

TEST(AudioEngine, BlocksRenderedCountsTicksAndSurvivesResetMeters) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  for (int i = 0; i < 5; ++i) ASSERT_EQ(sf_audio_engine_tick(f.e, 32), SF_OK);
  ASSERT_EQ(meter(f.e)["blocksRendered"], 5u);

  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  const json after = meter(f.e);
  EXPECT_EQ(after["blocksRendered"], 5u);  // reset_meters does NOT touch it
  EXPECT_EQ(after["truePeakLinear"][0], 0.0);  // latches zeroed
  EXPECT_EQ(after["truePeakDb"][0], nullptr);  // silent -> JSON null
  EXPECT_EQ(after["clipped"][0], false);
}

TEST(AudioEngine, ResetMetersZeroesLatchesButNotTails) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);
  f.io.dc = 0.5f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  for (int i = 0; i < 8; ++i) ASSERT_EQ(sf_audio_engine_tick(f.e, 128), SF_OK);
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 128), SF_OK);

  // The tail is preserved, so the very next block is in steady state (no
  // cold-start onset inflation): true peak ~= 0.5.
  EXPECT_NEAR(meter(f.e)["truePeakLinear"][0].get<double>(), 0.5, 1e-3);
}

TEST(AudioEngine, SilenceInputGivesSilenceAndNullDb) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);
  f.io.dc = 0.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  ASSERT_EQ(sf_audio_engine_tick(f.e, 128), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);
  const json m = meter(f.e);
  EXPECT_EQ(m["truePeakLinear"][0], 0.0);
  EXPECT_EQ(m["truePeakDb"][0], nullptr);
  EXPECT_EQ(m["clipped"][0], false);
}

TEST(AudioEngine, ReadFailureRendersSilenceAndWriteErrorIsIgnored) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);
  f.io.read_fail = true;
  f.io.write_fail.store(1);
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  // read SF_E_IO -> silence; write SF_E_IO -> skipped, never propagated.
  EXPECT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);
  EXPECT_EQ(meter(f.e)["blocksRendered"], 1u);  // meter updated regardless
}

TEST(AudioEngine, NullIoCallbacksAreSilentAndValid) {
  Fixture f;
  ASSERT_TRUE(f.make());

  sf_audio_engine_config_t cfg{};
  cfg.sample_rate = 44100;
  cfg.channels = 2;
  cfg.max_block_frames = 512;
  cfg.io.user = nullptr;
  cfg.io.read = nullptr;
  cfg.io.write = nullptr;
  ASSERT_EQ(sf_audio_engine_configure(f.e, &cfg), SF_OK);
  set_chain(f.p, -6.0);
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  EXPECT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
  EXPECT_EQ(meter(f.e)["blocksRendered"], 1u);
}

TEST(AudioEngine, DeterministicTicksAreByteIdenticalAcrossEngines) {
  const auto run = [] {
    Fixture f;
    EXPECT_TRUE(f.make());
    f.configure_default();
    set_chain(f.p, -3.0);
    f.io.dc = 0.25f;
    EXPECT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
    EXPECT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);
    std::vector<float> all;
    for (int i = 0; i < 16; ++i) {
      EXPECT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
      all.insert(all.end(), f.io.last_l, f.io.last_l + 64);
    }
    return all;
  };
  const std::vector<float> a = run();
  const std::vector<float> b = run();
  ASSERT_EQ(a.size(), b.size());
  EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size() * sizeof(float)));
}

// ---------------------------------------------------------------------------
// Plan validity: unknown target / no output / cyclic graph
// ---------------------------------------------------------------------------

TEST(AudioEngine, UnknownTargetRendersSilenceWithPlanValid) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "does-not-exist"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  ASSERT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);  // silence
  // The plan compiled (unknown target is not a compile failure).
  EXPECT_EQ(meter(f.e)["planValid"], true);
}

TEST(AudioEngine, NullAndEmptyOutputAreValidSilentConfigs) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);
  f.io.dc = 1.0f;

  ASSERT_EQ(sf_audio_engine_set_output(f.e, nullptr), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);
  EXPECT_EQ(meter(f.e)["planValid"], true);
}

TEST(AudioEngine, CyclicGraphStartIsNonFatalAndRendersSilence) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_cycle(f.p);
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "a"), SF_OK);

  // Snapshot #0 compile failure is non-fatal: the engine still starts.
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);
  EXPECT_EQ(meter(f.e)["planValid"], false);
}

// ---------------------------------------------------------------------------
// SEC-G4-01 meter buffer contract
// ---------------------------------------------------------------------------

TEST(AudioEngine, MeterJsonArgumentChecks) {
  Fixture f;
  ASSERT_TRUE(f.make());
  char buf[256];
  EXPECT_EQ(sf_audio_engine_meter_json(f.e, nullptr, sizeof(buf)), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_audio_engine_meter_json(f.e, buf, 0), SF_E_INVALID_ARG);
}

TEST(AudioEngine, MeterJsonBufferSizingAndNullTermination) {
  Fixture f;
  ASSERT_TRUE(f.make());

  char big[1024];
  ASSERT_EQ(sf_audio_engine_meter_json(f.e, big, sizeof(big)), SF_OK);
  const std::size_t len = std::strlen(big);  // payload length, no NUL

  // Exact fit (payload + NUL) succeeds.
  std::vector<char> exact(len + 1, '\x7f');
  EXPECT_EQ(sf_audio_engine_meter_json(f.e, exact.data(), exact.size()), SF_OK);
  EXPECT_EQ(std::strlen(exact.data()), len);

  // One byte short: SF_E_NOMEM and buf[0] == '\0' (never a partial payload).
  std::vector<char> small(len, '\x7f');
  EXPECT_EQ(sf_audio_engine_meter_json(f.e, small.data(), small.size()), SF_E_NOMEM);
  EXPECT_EQ(small[0], '\0');
}

TEST(AudioEngine, MeterJsonIsWellFormedWithDocumentedKeys) {
  Fixture f;
  ASSERT_TRUE(f.make());
  const json m = meter(f.e);
  EXPECT_EQ(m["channels"], 2);
  EXPECT_EQ(m["oversample"], 4);
  EXPECT_TRUE(m.contains("blocksRendered"));
  EXPECT_TRUE(m.contains("truePeakLinear"));
  EXPECT_TRUE(m.contains("truePeakDb"));
  EXPECT_TRUE(m.contains("clipped"));
  EXPECT_TRUE(m.contains("planValid"));
  EXPECT_EQ(m["truePeakLinear"].size(), 2u);
  EXPECT_EQ(m["truePeakDb"].size(), 2u);
  EXPECT_EQ(m["clipped"].size(), 2u);
  EXPECT_EQ(m["truePeakDb"][0], nullptr);  // never measured -> null
}

TEST(AudioEngine, MeterJsonIsLocaleIndependent) {
  Fixture f;
  ASSERT_TRUE(f.make());

  // Baseline: the output under the C locale (the reference form; to_chars is
  // locale-independent, so any locale must reproduce it byte-for-byte).
  char c_locale[1024];
  ASSERT_EQ(sf_audio_engine_meter_json(f.e, c_locale, sizeof(c_locale)), SF_OK);

  const char* loc = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
  if (loc == nullptr) loc = std::setlocale(LC_NUMERIC, "de_DE");
  if (loc == nullptr) {
    GTEST_SKIP() << "de_DE locale not installed; skipping locale-independence check";
  }

  char de_locale[1024];
  ASSERT_EQ(sf_audio_engine_meter_json(f.e, de_locale, sizeof(de_locale)), SF_OK);
  std::setlocale(LC_NUMERIC, "C");  // explicit restore (de_DE was applied)

  EXPECT_STREQ(c_locale, de_locale)
      << "meter_json must not depend on LC_NUMERIC (no comma decimal separators)";
}

// ---------------------------------------------------------------------------
// PACE: pacer progress, tick guard, bounded reap, concurrent meter reads
// ---------------------------------------------------------------------------

TEST(AudioEngine, PacerAdvancesBlocksAndTickIsRejected) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);
  f.io.dc = 0.1f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, SF_AUDIO_ENGINE_PACE), SF_OK);

  ASSERT_TRUE(wait_until([&] { return meter(f.e)["blocksRendered"].get<uint64_t>() > 0; },
                         20000))
      << "pacer never advanced blocksRendered";

  EXPECT_EQ(sf_audio_engine_tick(f.e, 64), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), "audio engine: tick: pacer active");

  ASSERT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_join(f.e), SF_OK);  // bounded reap

  // The pacer is joined: the count is settled.
  const uint64_t settled = meter(f.e)["blocksRendered"].get<uint64_t>();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(meter(f.e)["blocksRendered"].get<uint64_t>(), settled);
}

TEST(AudioEngine, MeterJsonAndResetMetersAreSafeAgainstPacer) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, -6.0);
  f.io.dc = 0.5f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, SF_AUDIO_ENGINE_PACE), SF_OK);

  std::atomic<bool> go{true};
  std::thread reader([&] {
    char buf[1024];
    while (go.load(std::memory_order_relaxed)) {
      (void)sf_audio_engine_meter_json(f.e, buf, sizeof(buf));
      (void)sf_audio_engine_reset_meters(f.e);
    }
  });

  // Let the reader contend with the pacer for a while (SEC-G4-02).
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  go.store(false, std::memory_order_relaxed);
  if (reader.joinable()) reader.join();

  ASSERT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_join(f.e), SF_OK);
  EXPECT_GT(meter(f.e)["blocksRendered"].get<uint64_t>(), 0u);
}

// ---------------------------------------------------------------------------
// D6: last_report passthrough
// ---------------------------------------------------------------------------

TEST(AudioEngine, LastReportPassthroughByteIdenticalAndMeterFree) {
  // Project A: engine; Project B: a plain runner. Same graph => the SAME
  // evaluate_mixer dump must come back from both last_report surfaces.
  sf_project_t* pa = sf_project_create("A", nullptr);
  sf_project_t* pb = sf_project_create("B", nullptr);
  ASSERT_NE(pa, nullptr);
  ASSERT_NE(pb, nullptr);
  set_chain(pa, -6.0);
  set_chain(pb, -6.0);

  sf_cmd_queue_t* qa = nullptr;
  sf_cmd_queue_t* qb = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&qa), SF_OK);
  ASSERT_EQ(sf_cmd_queue_create(&qb), SF_OK);

  sf_audio_engine_t* e = nullptr;
  ASSERT_EQ(sf_audio_engine_create(&e, qa, pa), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, qb, pb), SF_OK);

  ASSERT_EQ(sf_audio_engine_start(e, 0), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  sf_cmd_t eval{};
  eval.type = SF_CMD_EVALUATE_MIXER;
  ASSERT_EQ(sf_cmd_queue_enqueue(qa, &eval), SF_OK);
  ASSERT_EQ(sf_cmd_queue_enqueue(qb, &eval), SF_OK);

  char ea[8192] = {};
  char rb[8192] = {};
  ASSERT_TRUE(wait_until(
      [&] { return sf_audio_engine_last_report(e, ea, sizeof(ea)) == SF_OK; }, 20000));
  ASSERT_TRUE(wait_until(
      [&] { return sf_queue_runner_last_report(r, rb, sizeof(rb)) == SF_OK; }, 20000));

  EXPECT_EQ(0, std::strcmp(ea, rb)) << "engine last_report is not a passthrough";
  // D6: the meter surface never leaks into last_report.
  EXPECT_EQ(std::string::npos, std::string(ea).find("truePeak"));

  sf_queue_runner_stop(r);
  sf_queue_runner_join(r);
  sf_queue_runner_destroy(r);
  sf_audio_engine_stop(e);
  sf_audio_engine_join(e);
  sf_audio_engine_destroy(e);
  sf_cmd_queue_destroy(qa);
  sf_cmd_queue_destroy(qb);
  sf_project_destroy(pa);
  sf_project_destroy(pb);
}

TEST(AudioEngine, LastReportBeforeEvaluateIsIoError) {
  Fixture f;
  ASSERT_TRUE(f.make());
  char buf[128];
  EXPECT_EQ(sf_audio_engine_last_report(f.e, buf, sizeof(buf)), SF_E_IO);
  // The passthrough preserves the runner's contract exactly: the runner reports
  // on the THREAD-LOCAL store (sf_queue_runner_last_report uses set_last_error).
  EXPECT_STREQ(sf_last_error(nullptr), "runner.lastReport: no report yet");
  EXPECT_EQ(sf_audio_engine_last_report(nullptr, buf, sizeof(buf)), SF_E_INVALID_ARG);
}
