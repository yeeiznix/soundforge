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
#include "soundforge/sf_graph.h"
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

// src(src_db) -> out(out_db), for live-control test with two observable targets
// (each node block applies its own gain; src(-6dB)->out(+6dB) yields src≈0.501187,
// out≈1.0, both @1e-6 per the amended P5 acceptance).
void set_chain_gains(sf_project_t* p, double src_db, double out_db) {
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  ip->doc.signalGraph.nodes.clear();
  ip->doc.signalGraph.edges.clear();
  ip->doc.signalGraph.nodes.push_back(mk_node("src", sfcore::SfNodeSource, src_db));
  ip->doc.signalGraph.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, out_db));
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
// G5 P4 — project-destroy skip-free guard (G4-8, mirrors SEC-G3-2). An engine
// borrows `proj` even in CREATED state (its runner never started), so freeing
// the project under an attached engine is a latent UAF. sf_project_destroy is
// void, so it skips the free + sets the handle error; the caller must
// sf_audio_engine_destroy first, then destroy the project again.
// ---------------------------------------------------------------------------

// Flow (a): engine destroyed first -> the claim is released -> the project
// destroy takes the normal free path with no guard error.
TEST(AudioEngine, ProjectDestroyAfterEngineDestroySucceeds) {
  sf_project_t* p = sf_project_create("E", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_audio_engine_t* e = nullptr;
  ASSERT_EQ(sf_audio_engine_create(&e, q, p), SF_OK);

  // Engine destroyed FIRST (CREATED state; no runner/pacer thread to stop):
  // the audioEngine claim is released before the engine free.
  ASSERT_EQ(sf_audio_engine_destroy(e), SF_OK);
  // No guard fired: the handle error is still the pristine sentinel.
  EXPECT_STREQ(sf_last_error(p), "no error");

  // The project destroy now takes the normal free path. Do not touch `p`
  // after this point.
  sf_project_destroy(p);
  sf_cmd_queue_destroy(q);
}

// Flow (b): destroy-project WITHOUT engine destroy -> the void guard skips the
// free + sets the attached-engine message; after sf_audio_engine_destroy the
// claim is cleared and the second project destroy succeeds.
TEST(AudioEngine, ProjectDestroySkippedWhileEngineAttachedThenRetrySucceeds) {
  sf_project_t* p = sf_project_create("E", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_audio_engine_t* e = nullptr;
  ASSERT_EQ(sf_audio_engine_create(&e, q, p), SF_OK);

  // Engine is CREATED only: no runner ever started, no pacer thread.
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  ASSERT_EQ(ip->runnerState.load(std::memory_order_acquire),
            static_cast<int32_t>(sfcore::SfRunnerIdle));
  ASSERT_EQ(ip->audioEngine.load(std::memory_order_acquire), static_cast<void*>(e));

  // destroy-project BEFORE destroy-engine: the void API skips the free and
  // signals on the handle error store.
  sf_project_destroy(p);
  EXPECT_STREQ(sf_last_error(p), "project.destroy: audio engine attached");

  // The handle was NOT freed: synchronous reads still work (runner is IDLE, so
  // they are not rejected) and the engine claim still owns the slot.
  EXPECT_EQ(sf_project_get_schema_version(p), SF_SCHEMA_VERSION);
  EXPECT_STREQ(sf_project_get_name(p), "E");
  EXPECT_EQ(ip->audioEngine.load(std::memory_order_acquire), static_cast<void*>(e));

  // Release the engine first (this clears the claim)...
  ASSERT_EQ(sf_audio_engine_destroy(e), SF_OK);
  EXPECT_EQ(ip->audioEngine.load(std::memory_order_acquire), nullptr);

  // ...then the mandatory SECOND project destroy proceeds (normal free). Do
  // not touch `p` after this point.
  sf_project_destroy(p);
  sf_cmd_queue_destroy(q);
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

// ---------------------------------------------------------------------------
// P5 — live-loop e2e (PLAN_G4 §6 P5). Proves the full loop: queue mutation ->
// runner publish observer -> snapshot store -> next tick renders the new plan;
// the true-peak latch's hold (non-decaying) semantics; and the busy read guard
// while the engine's internal runner is RUNNING.
// ---------------------------------------------------------------------------
namespace {

sf_cmd_t p5_make_cmd(int32_t type) {
  sf_cmd_t c{};
  c.type = type;
  return c;
}

sf_cmd_t p5_make_set_mixer(const char* node_id, double gain_db, int32_t flags) {
  sf_cmd_t c{};
  c.type = SF_CMD_SET_MIXER;
  std::strncpy(c.id1, node_id, sizeof(c.id1) - 1);
  c.value = gain_db;  // gain_db
  c.value2 = 0.0;     // pan
  c.flags = flags;    // bit0 mute
  return c;
}

// Deterministic drain, NO fixed sleeps: enqueue a STOP barrier behind the
// mutation(s) already queued and poll for the runner epilogue. On the runner
// thread the mutation is applied and the publish observer fires (publishing
// the newest snapshot into the store) BEFORE the epilogue's release-store of
// SfRunnerStopped, so observing STOPPED guarantees the mutation is applied,
// the snapshot published and the doc quiescent. The engine itself stays
// RUNNING (only its internal runner stopped), so further ticks still work.
void p5_drain(sf_cmd_queue_t* q, sf_project_t* p) {
  sf_cmd_t stop = p5_make_cmd(SF_CMD_STOP);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &stop), SF_OK);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  ASSERT_TRUE(wait_until(
      [&] {
        return ip->runnerState.load(std::memory_order_acquire) ==
               static_cast<int32_t>(sfcore::SfRunnerStopped);
      },
      30000)) << "runner never reached STOPPED after STOP barrier";
}

bool p5_audit_has(const sfcore::SfProject* ip, const char* action) {
  for (const auto& a : ip->doc.auditLog) {
    if (a.action == action) return true;
  }
  return false;
}

}  // namespace

// Acceptances 1 + 2 + 6 + 7: no-PACE graph source(-6dB)->output renders 0.5
// with meter 0.5; a queued SF_CMD_SET_MIXER(0dB) while RUNNING is drained
// deterministically and the NEXT tick renders 1.0 with the latch advancing
// toward full scale (the interpolator rings the step so the transition peak is
// ~1.0628, clipped true; a settle tick pins the steady 1.0); no project.migrate
// audit entry, schemaVersion 2; destroy after the session is clean.
TEST(AudioEngine, E2eGainMutationRendersNextTickAndLatchAdvancesToFullScale) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, -6.0);  // 10^(-6/20) = 0.501187
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  // Acceptance 1: prime, reset latches (tails kept), then one steady tick.
  for (int i = 0; i < 8; ++i) ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  const float half = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  EXPECT_NEAR(f.io.last_l[0], half, 1e-6f);
  EXPECT_NEAR(f.io.last_r[0], half, 1e-6f);
  const json m0 = meter(f.e);
  EXPECT_NEAR(m0["truePeakLinear"][0].get<double>(), 0.501187, 1e-3);
  EXPECT_NEAR(m0["truePeakLinear"][1].get<double>(), 0.501187, 1e-3);
  EXPECT_EQ(m0["clipped"][0], false);
  EXPECT_EQ(m0["blocksRendered"], 9u);

  // Acceptance 2: enqueue SET_MIXER(0dB) while RUNNING, drain via STOP
  // barrier, then the next tick renders 1.0 and the latch advances.
  sf_cmd_t set0 = p5_make_set_mixer("src", 0.0, 0);
  ASSERT_EQ(sf_cmd_queue_enqueue(f.q, &set0), SF_OK);
  ASSERT_NO_FATAL_FAILURE(p5_drain(f.q, f.p));
  auto* ip = reinterpret_cast<sfcore::SfProject*>(f.p);
  EXPECT_EQ(ip->doc.signalGraph.nodes[0].mixer.gainDb, 0.0);  // applied

  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  EXPECT_NEAR(f.io.last_l[0], 1.0f, 1e-6f);
  const json m1 = meter(f.e);
  EXPECT_NEAR(m1["truePeakLinear"][0].get<double>(), 1.062817, 2e-3);
  EXPECT_EQ(m1["clipped"][0], true);
  EXPECT_EQ(m1["blocksRendered"], 10u);

  // Settle tick pins the steady full-scale number: reset latches (tails kept),
  // one more tick -> latch ~1.0, still clipped.
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  const json m2 = meter(f.e);
  EXPECT_NEAR(m2["truePeakLinear"][0].get<double>(), 1.0, 1e-3);
  EXPECT_EQ(m2["clipped"][0], true);

  // Acceptance 6: no project.migrate audit entry; schemaVersion is 2.
  EXPECT_FALSE(p5_audit_has(ip, "project.migrate"));
  EXPECT_TRUE(p5_audit_has(ip, "graph.setMixer"));
  EXPECT_EQ(ip->doc.schemaVersion, SF_SCHEMA_VERSION);

  // Acceptance 7: destroy after the e2e session is clean (stop+join+destroy
  // all succeed; f.e cleared so the Fixture dtor does not double-free).
  ASSERT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_join(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_destroy(f.e), SF_OK);
  f.e = nullptr;
}

// Acceptance 3: a mute mutation renders silence on the next tick while the
// true-peak latch HOLDS the previous peak (non-decaying): the interpolator
// rings the pre-mute tail into the silent block (~0.5643, above the pre-mute
// 0.501187), and a second fully-silent block (peak 0.0) leaves the latch
// bit-identical — the latch never decays.
TEST(AudioEngine, E2eMuteMutationRendersSilenceAndLatchHoldsNonDecaying) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, -6.0);
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  // Prime + measure the pre-mute steady state (acceptance 3 baseline).
  for (int i = 0; i < 8; ++i) ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  const double pre_mute = meter(f.e)["truePeakLinear"][0].get<double>();
  EXPECT_NEAR(pre_mute, 0.501187, 1e-3);

  // Mute mutation (gain stays -6dB; SF_MIXER_FLAG_MUTE) -> deterministic drain.
  sf_cmd_t mute = p5_make_set_mixer("src", -6.0, SF_MIXER_FLAG_MUTE);
  ASSERT_EQ(sf_cmd_queue_enqueue(f.q, &mute), SF_OK);
  ASSERT_NO_FATAL_FAILURE(p5_drain(f.q, f.p));
  auto* ip = reinterpret_cast<sfcore::SfProject*>(f.p);
  EXPECT_EQ(ip->doc.signalGraph.nodes[0].mixer.mute, true);

  // Next tick: silence at the output; latch HOLDS a peak above the pre-mute
  // value (ring carryover), not clipped.
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);
  const json m1 = meter(f.e);
  EXPECT_NEAR(m1["truePeakLinear"][0].get<double>(), 0.564303, 2e-3);
  EXPECT_GT(m1["truePeakLinear"][0].get<double>(), pre_mute);
  EXPECT_EQ(m1["clipped"][0], false);
  const double held = m1["truePeakLinear"][0].get<double>();

  // A second fully-silent block (peak 0.0) must NOT decay the latch: the held
  // value is bit-identical (non-decaying max latch).
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);
  EXPECT_EQ(meter(f.e)["truePeakLinear"][0].get<double>(), held);
}

// Acceptances 4 + 5: while the engine's internal runner is RUNNING, doc-
// touching reads reject with SF_E_IO + the verbatim "project.busy: queue
// runner active" string (the exact sf_graph_validate call from the P4b busy
// tests). After stop+join the handle is usable synchronously (G3 C8).
TEST(AudioEngine, E2eReadGuardBusyWhileRunningThenSynchronousAfterJoin) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, -6.0);
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  constexpr const char* kBusy = "project.busy: queue runner active";
  char report[4096];
  char* js = nullptr;
  size_t jlen = 0;

  // Acceptance 4: busy rejects while the internal runner is RUNNING.
  EXPECT_EQ(sf_graph_validate(f.p, report, sizeof(report)), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(sf_project_to_json(f.p, &js, &jlen), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(js, nullptr);
  EXPECT_EQ(sf_project_get_schema_version(f.p), -1);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);

  // Acceptance 5: after stop+join the handle is usable synchronously (C8).
  ASSERT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_join(f.e), SF_OK);
  EXPECT_EQ(sf_project_to_json(f.p, &js, &jlen), SF_OK);
  ASSERT_NE(js, nullptr);
  sf_free_string(js);
  EXPECT_EQ(sf_graph_validate(f.p, report, sizeof(report)), SF_OK);
  EXPECT_EQ(sf_project_get_schema_version(f.p), SF_SCHEMA_VERSION);
}

// ---------------------------------------------------------------------------
// G5 P1 — SF_CMD_SET_OUTPUT (cmd 8): public constant + runner interception
// (PLAN_G5 §3.1 D1, §3.3 D3 + SEC-G5-01)
// ---------------------------------------------------------------------------

// The new command type is a public, runtime-only constant: cmd 8, no slot
// growth (the wire/slot image stays 176 B) and it round-trips through the
// queue with its type + id1 payload preserved.
TEST(AudioEngine, SetOutputCommandConstantRoundTripsThroughQueue) {
  EXPECT_EQ(SF_CMD_SET_OUTPUT, 8);
  EXPECT_EQ(sizeof(sf_cmd_t), 176u);  // no wire/slot growth

  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_cmd_t in{};
  in.type = SF_CMD_SET_OUTPUT;
  std::strncpy(in.id1, "out", sizeof(in.id1) - 1);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &in), SF_OK);

  sf_cmd_t out{};
  ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
  EXPECT_EQ(out.type, SF_CMD_SET_OUTPUT);
  EXPECT_STREQ(out.id1, "out");
  EXPECT_EQ(sf_cmd_queue_depth(q), 0);
  sf_cmd_queue_destroy(q);
}

// SEC-G5-01 negative test (P1 acceptance): an id1 with no NUL in all 64 bytes
// must NOT escape the field as an unbounded scan. The runner bounds the copy
// with strnlen(id1, sizeof(id1)); the resulting unknown target compiles to a
// valid plan that renders silence (SEC-G5-02), so the drain completes with no
// crash/overread and the engine keeps its known behavior. A control command is
// never applied as a mutation: no audit entry, schemaVersion unchanged.
TEST(AudioEngine, SetOutputCommandUnterminatedIdDrainsSafely) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());
  set_chain(f.p, 0.0);  // gain 0 dB -> the "out" target renders 1.0
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  // Baseline: the static target renders the source block.
  ASSERT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
  EXPECT_NEAR(f.io.last_l[0], 1.0f, 1e-6f);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(f.p);
  const size_t audit_before = ip->doc.auditLog.size();

  sf_cmd_t cmd{};
  cmd.type = SF_CMD_SET_OUTPUT;
  std::memset(cmd.id1, 'A', sizeof(cmd.id1));  // 64 bytes, no NUL terminator
  ASSERT_EQ(sf_cmd_queue_enqueue(f.q, &cmd), SF_OK);

  // Deterministic STOP-barrier drain: the interception ran, the engine-side
  // callback re-published a plan with the (bounded) unknown target.
  ASSERT_NO_FATAL_FAILURE(p5_drain(f.q, f.p));

  ASSERT_EQ(sf_audio_engine_tick(f.e, 64), SF_OK);
  EXPECT_EQ(f.io.last_l[0], 0.0f);  // unknown target -> silence, no overread
  EXPECT_EQ(f.io.last_r[0], 0.0f);
  EXPECT_EQ(meter(f.e)["planValid"], true);  // unknown target is not a compile failure

  // Control command: no mutation -> no audit entry, no schema change.
  EXPECT_EQ(ip->doc.auditLog.size(), audit_before);
  EXPECT_FALSE(p5_audit_has(ip, "project.migrate"));
  EXPECT_EQ(ip->doc.schemaVersion, SF_SCHEMA_VERSION);
}

// ---------------------------------------------------------------------------
// G5 P5 — live-control end-to-end: SF_CMD_SET_OUTPUT retargets while RUNNING
// (PLAN_G5 §6.1 amended acceptance: two engine sessions, one drain each)
// ---------------------------------------------------------------------------

// Session A (target unchanged): benign same-target retarget renders the output
// node block with its own gain; the latch is still live (observes gain advance).
TEST(AudioEngine, LiveControlSameTargetRetargetBenign) {
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());

  // Fixture: src(-6 dB) -> out(+6 dB). Target "out" renders input × src-gain ×
  // out-gain = 1.0 × 0.501187 × 1.995262 ≈ 1.0 (both @1e-6 per F1).
  set_chain_gains(f.p, -6.0, 6.0);  // 10^(-6/20) × 10^(+6/20) ≈ 1.0
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  // Prime + measure steady state (target "out").
  for (int i = 0; i < 8; ++i) ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);

  // Session A: same-target retarget (out -> out) via SF_CMD_SET_OUTPUT.
  sf_cmd_t set_out{};
  set_out.type = SF_CMD_SET_OUTPUT;
  std::strncpy(set_out.id1, "out", sizeof(set_out.id1) - 1);
  ASSERT_EQ(sf_cmd_queue_enqueue(f.q, &set_out), SF_OK);
  ASSERT_NO_FATAL_FAILURE(p5_drain(f.q, f.p));

  // Next tick: still targets "out", still ≈1.0.
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  EXPECT_NEAR(f.io.last_l[0], 1.0f, 1e-6f);
  EXPECT_NEAR(f.io.last_r[0], 1.0f, 1e-6f);
  const json ma = meter(f.e);
  EXPECT_NEAR(ma["truePeakLinear"][0].get<double>(), 1.0, 1e-3);
  EXPECT_NEAR(ma["truePeakLinear"][1].get<double>(), 1.0, 1e-3);

  // Control command: no mutation -> no audit entry, no schema change.
  auto* ip = reinterpret_cast<sfcore::SfProject*>(f.p);
  EXPECT_FALSE(p5_audit_has(ip, "project.migrate"));
  EXPECT_EQ(ip->doc.schemaVersion, SF_SCHEMA_VERSION);

  ASSERT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_join(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_destroy(f.e), SF_OK);
  f.e = nullptr;
}

// Session B (retarget observable): live output retarget from "out" to "src"
// changes the rendered amplitude; the meter advances to track the new target.
// The latch is non-decaying, so we reset it between measuring targets.
TEST(AudioEngine, LiveControlRetargetObservableAmplitudeAndMeterAdvance) {
  // Fresh engine for session B retarget observable test.
  Fixture f;
  ASSERT_TRUE(f.make());
  ASSERT_NO_FATAL_FAILURE(f.configure_default());

  // Fixture: src(-6 dB) -> out(+6 dB). Target "out" ≈1.0, target "src" ≈0.501187.
  set_chain_gains(f.p, -6.0, 6.0);
  f.io.dc = 1.0f;
  ASSERT_EQ(sf_audio_engine_set_output(f.e, "out"), SF_OK);
  ASSERT_EQ(sf_audio_engine_start(f.e, 0), SF_OK);

  // Prime + measure initial state (target "out" ≈ 1.0).
  for (int i = 0; i < 8; ++i) ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);

  const json m_before = meter(f.e);
  EXPECT_NEAR(m_before["truePeakLinear"][0].get<double>(), 1.0, 1e-3);

  // Retarget from "out" to "src" via SF_CMD_SET_OUTPUT.
  sf_cmd_t set_src{};
  set_src.type = SF_CMD_SET_OUTPUT;
  std::strncpy(set_src.id1, "src", sizeof(set_src.id1) - 1);
  ASSERT_EQ(sf_cmd_queue_enqueue(f.q, &set_src), SF_OK);
  ASSERT_NO_FATAL_FAILURE(p5_drain(f.q, f.p));

  // Next tick: now targets "src", renders ≈0.501187. The interpolator may ring
  // from the out-gain tail on the first transition block, so we tick again.
  // Then reset meters (tails kept) to clear the transient peak.
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);
  ASSERT_EQ(sf_audio_engine_reset_meters(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_tick(f.e, 256), SF_OK);

  const float expect_src = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
  EXPECT_NEAR(f.io.last_l[0], expect_src, 1e-6f);
  EXPECT_NEAR(f.io.last_r[0], expect_src, 1e-6f);

  // Latch now measures the settled target: peak ≈0.501187; the latch now tracks
  // the source target after the retarget.
  const json m_after = meter(f.e);
  EXPECT_NEAR(m_after["truePeakLinear"][0].get<double>(), 0.501187, 1e-3);
  EXPECT_NEAR(m_after["truePeakLinear"][1].get<double>(), 0.501187, 1e-3);

  // Control command: no mutation -> no audit entry, no schema change.
  auto* ip = reinterpret_cast<sfcore::SfProject*>(f.p);
  EXPECT_FALSE(p5_audit_has(ip, "project.migrate"));
  EXPECT_EQ(ip->doc.schemaVersion, SF_SCHEMA_VERSION);

  ASSERT_EQ(sf_audio_engine_stop(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_join(f.e), SF_OK);
  ASSERT_EQ(sf_audio_engine_destroy(f.e), SF_OK);
  f.e = nullptr;
}
