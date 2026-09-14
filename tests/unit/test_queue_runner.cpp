// SoundForge G3 P4a — sf_queue_runner lifecycle + live drain lane (PLAN_G3
// §6 P4a, §7.1). Covers: one-shot lifecycle create→start→stop→join→destroy;
// the 10k-command SPSC drain through apply_batch_impl (audit FIFO cap + gapless
// seq watermark); the STOP(0) barrier (ADD→STOP→ADD ordering); EVALUATE_MIXER
// (7) interception → bounded ≤8 KiB last_report with the "_truncated": true
// sentinel; batch partial-failure dropping (ERROR row, handle error untouched —
// SEC-G3-4); the C7/single-owner destroy guard (RUNNING + STOPPING window);
// join bounds + idempotence; last_report argument/state checks.
#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_command_queue.h"
#include "soundforge/sf_diagnostics.h"
#include "soundforge/sf_graph.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_queue_runner.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace {

sf_cmd_t make_cmd(int32_t type) {
  sf_cmd_t c{};
  c.type = type;
  return c;
}

sf_cmd_t make_add(const char* name, int32_t kind) {
  sf_cmd_t c = make_cmd(SF_CMD_ADD_NODE);
  c.port_a = kind;
  std::strncpy(c.id1, name, sizeof(c.id1) - 1);
  return c;
}

const sfcore::SignalNode* find_by_name(sfcore::SfProject* ip, const std::string& name) {
  for (const auto& n : ip->doc.signalGraph.nodes) {
    if (n.name == name) return &n;
  }
  return nullptr;
}

// Poll a predicate until it holds or the deadline passes. Deadlines are
// generous: UBSan builds and 1-core proot boxes are ~10x slower than native.
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

int32_t runner_state(sfcore::SfProject* ip) {
  return ip->runnerState.load(std::memory_order_acquire);
}

}  // namespace

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

TEST(QueueRunner, CreateNullArguments) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);

  EXPECT_EQ(sf_queue_runner_create(nullptr, q, p), SF_E_INVALID_ARG);
  sf_queue_runner_t* r = nullptr;
  EXPECT_EQ(sf_queue_runner_create(&r, nullptr, p), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_queue_runner_create(&r, q, nullptr), SF_E_INVALID_ARG);

  sf_project_destroy(p);
  sf_cmd_queue_destroy(q);
}

TEST(QueueRunner, StartStopJoinNullArguments) {
  EXPECT_EQ(sf_queue_runner_start(nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_queue_runner_stop(nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_queue_runner_join(nullptr), SF_E_INVALID_ARG);
}

TEST(QueueRunner, LastReportArgumentAndStateChecks) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  char buf[64];
  EXPECT_EQ(sf_queue_runner_last_report(nullptr, buf, sizeof(buf)), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_queue_runner_last_report(r, nullptr, sizeof(buf)), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_queue_runner_last_report(r, buf, 0), SF_E_INVALID_ARG);
  // No EVALUATE has ever completed on this runner.
  EXPECT_EQ(sf_queue_runner_last_report(r, buf, sizeof(buf)), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), "runner.lastReport: no report yet");

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// Lifecycle state machine
// ---------------------------------------------------------------------------

TEST(QueueRunner, LifecycleCreateStartStopJoinDestroy) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);

  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(runner_state(ip), static_cast<int32_t>(sfcore::SfRunnerIdle));

  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  // The CAS precedes the spawn, so RUNNING is visible as soon as start returns.
  EXPECT_EQ(runner_state(ip), static_cast<int32_t>(sfcore::SfRunnerRunning));

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  // join() happens-after the runner epilogue's STOPPED store (thread-exit
  // synchronization), so STOPPED is settled and the handle is caller-owned.
  EXPECT_EQ(runner_state(ip), static_cast<int32_t>(sfcore::SfRunnerStopped));

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, JoinAndStopAreNoopsBeforeStart) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  EXPECT_EQ(sf_queue_runner_join(r), SF_OK);  // never started → safe no-op
  EXPECT_EQ(sf_queue_runner_stop(r), SF_OK);  // not RUNNING → idempotent no-op
  EXPECT_EQ(runner_state(reinterpret_cast<sfcore::SfProject*>(p)),
            static_cast<int32_t>(sfcore::SfRunnerIdle));
  EXPECT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, DoubleStartRejectedByCas) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  EXPECT_EQ(sf_queue_runner_start(r), SF_E_IO);
  EXPECT_STREQ(sf_last_error(p), "project.runner: already active");

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, StartFromStoppedRejectedOneShot) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);  // NOW STOPPED; nothing resets it

  EXPECT_EQ(sf_queue_runner_start(r), SF_E_IO);
  EXPECT_STREQ(sf_last_error(p), "project.runner: already active");

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, CreateRejectsWhenProjectNotIdle) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);  // project is STOPPED

  // A project handle is single-runner for its lifetime: no second create.
  sf_queue_runner_t* r2 = reinterpret_cast<sf_queue_runner_t*>(0x1);
  EXPECT_EQ(sf_queue_runner_create(&r2, q, p), SF_E_IO);
  EXPECT_STREQ(sf_last_error(p), "runner.create: project not idle");
  EXPECT_EQ(r2, nullptr);  // *out cleared on the rejected path

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// destroy guard (SEC-G3-2): never free while the runner thread is alive
// ---------------------------------------------------------------------------

TEST(QueueRunner, DestroyNullIsSafeNoop) {
  EXPECT_EQ(sf_queue_runner_destroy(nullptr), SF_OK);
}

TEST(QueueRunner, DestroyRejectedWhileRunning) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  // Live runner: destroy must reject (handle NOT freed) and surface the
  // error on the project's handle error store.
  EXPECT_EQ(sf_queue_runner_destroy(r), SF_E_IO);
  EXPECT_STREQ(sf_last_error(p), "project.destroy: queue runner active");

  // The handle is still usable: stop → join → destroy completes the cycle.
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, DestroyRejectedWhileStopping) {
  // Deterministic STOPPING-window probe. The REAL stop()->destroy() window can
  // be a few microseconds (the epilogue publishes STOPPED as soon as the last
  // batch drains), so racing it would be flaky; instead we simulate an
  // in-flight STOPPING state directly (no thread involved: create never spawns
  // one) and verify the guard.
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  ip->runnerState.store(static_cast<int32_t>(sfcore::SfRunnerStopping), std::memory_order_release);
  EXPECT_EQ(sf_queue_runner_destroy(r), SF_E_IO);
  EXPECT_STREQ(sf_last_error(p), "project.destroy: queue runner active");
  EXPECT_EQ(sf_queue_runner_join(r), SF_OK);  // no thread was ever started

  ip->runnerState.store(static_cast<int32_t>(sfcore::SfRunnerIdle), std::memory_order_release);
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, DestroyAutoJoinsStoppedThread) {
  // destroy() must reap a started-but-unjoined thread itself; otherwise
  // std::thread's destructor would std::terminate (contract §7.1 row).
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);

  // Wait for the epilogue (STOPPED) WITHOUT joining, then let destroy join.
  ASSERT_TRUE(wait_until(
      [&] { return runner_state(ip) == static_cast<int32_t>(sfcore::SfRunnerStopped); }, 30000));
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);  // auto-join + free
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, JoinReapsWithinBoundedTime) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);

  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(elapsed_ms, 30000);  // bounded — a deadlock would hang the suite
  EXPECT_EQ(sf_queue_runner_join(r), SF_OK);  // idempotent re-join

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// Live drain lane
// ---------------------------------------------------------------------------

TEST(QueueRunner, TenKAddNodesDrains) {
  constexpr int N = 10000;
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  std::atomic<bool> producer_done{false};
  std::thread producer([&] {
    for (int i = 0; i < N; ++i) {
      if ((i % 256) == 0 && i > 0) std::this_thread::yield();  // ring pacing
      char name[16];
      std::snprintf(name, sizeof(name), "n%05d", i);
      sf_cmd_t c = make_add(name, SF_NODE_PROCESSOR);
      while (sf_cmd_queue_enqueue(q, &c) != SF_OK) {
        std::this_thread::yield();  // transient full → retry (G3 producer C2)
      }
    }
    producer_done = true;
  });

  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  ASSERT_TRUE(wait_until(
      [&] { return producer_done.load() && sf_cmd_queue_depth(q) == 0; }, 60000));
  producer.join();

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);

  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  EXPECT_EQ(ip->doc.signalGraph.nodes.size(), static_cast<size_t>(N));
  // Audit FIFO cap: 1 create + 10000 addNode entries → exactly 1000 kept.
  EXPECT_EQ(ip->doc.auditLog.size(), sfcore::kMaxAuditEntries);
  EXPECT_EQ(static_cast<std::string>(ip->doc.auditLog.back().action),
            "graph.addNode");
  EXPECT_EQ(static_cast<std::string>(ip->doc.auditLog.front().action),
            "graph.addNode");  // oldest create entry was dropped

  // No command was lost: the seq watermark continues at N+1 (C4).
  {
    sf_cmd_t probe = make_add("probe", SF_NODE_PROCESSOR);
    ASSERT_EQ(sf_cmd_queue_enqueue(q, &probe), SF_OK);
    sf_cmd_t out{};
    ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
    EXPECT_EQ(out.seq, static_cast<uint64_t>(N) + 1);
    EXPECT_STREQ(out.id1, "probe");
  }

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, StopBarrierLeavesCommandBehindQueued) {
  // ORC-2: a queued SF_CMD_STOP is a barrier — everything AHEAD of it drains,
  // everything BEHIND it stays queued, and the runner exits on its own (no
  // sf_queue_runner_stop() call needed).
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  sf_cmd_t a1 = make_add("a1", SF_NODE_SOURCE);
  sf_cmd_t a2 = make_add("a2", SF_NODE_OUTPUT);
  sf_cmd_t stop = make_cmd(SF_CMD_STOP);
  sf_cmd_t a3 = make_add("a3", SF_NODE_SOURCE);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &a1), SF_OK);   // seq 1
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &a2), SF_OK);   // seq 2
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &stop), SF_OK); // seq 3 — the barrier
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &a3), SF_OK);   // seq 4 — stays queued

  // join() returns only after the runner consumed the barrier and exited.
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);

  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  EXPECT_EQ(ip->doc.signalGraph.nodes.size(), 2u);  // a1 + a2 (ahead of STOP)
  EXPECT_EQ(find_by_name(ip, "a1") != nullptr, true);
  EXPECT_EQ(find_by_name(ip, "a2") != nullptr, true);

  EXPECT_EQ(sf_cmd_queue_depth(q), 1);  // a3 is behind the barrier
  sf_cmd_t out{};
  ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
  EXPECT_EQ(out.type, SF_CMD_ADD_NODE);
  EXPECT_EQ(out.seq, 4u);  // seq assigned at enqueue time is preserved
  EXPECT_STREQ(out.id1, "a3");

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, BatchPartialFailureDropsRemainder) {
  // A failed cmd stops apply_batch_impl mid-batch; commands already dequeued
  // into the same batch are dropped. The runner logs the §5.4 ERROR row and
  // does NOT exit — stop+join reaps it. SEC-G3-4: the handle error store is
  // never touched by the runner.
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  // Enqueue BEFORE start: the runner's first pass drains the whole queue
  // (3 < kRunnerBatch), so the batch is deterministically {good1, bad, good3}.
  sf_cmd_t good1 = make_add("good1", SF_NODE_SOURCE);
  sf_cmd_t bad = make_cmd(SF_CMD_ADD_NODE);  // empty name → add_node_impl rejects
  bad.port_a = SF_NODE_PROCESSOR;
  sf_cmd_t good3 = make_add("good3", SF_NODE_OUTPUT);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &good1), SF_OK);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &bad), SF_OK);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &good3), SF_OK);

  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);
  // The first pass drains the whole queue (3 < kRunnerBatch) — wait for the
  // dequeue before stopping so STOPPING cannot fire on an empty first pass.
  ASSERT_TRUE(wait_until([&] { return sf_cmd_queue_depth(q) == 0; }, 30000));
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);

  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  EXPECT_EQ(ip->doc.signalGraph.nodes.size(), 1u);
  EXPECT_NE(find_by_name(ip, "good1"), nullptr);
  EXPECT_EQ(find_by_name(ip, "good3"), nullptr);  // dropped with the failed batch
  EXPECT_EQ(sf_cmd_queue_depth(q), 0);            // nothing left queued
  EXPECT_STREQ(sf_last_error(p), "no error");     // runner never writes it

  // §5.4 ERROR row: "apply_batch stopped at 1/3: <err>".
  const std::string log_path = "runner_partial_fail.log";
  ASSERT_EQ(sf_flush_logs(log_path.c_str()), SF_OK);
  std::ifstream f(log_path);
  const std::string contents((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("apply_batch stopped at 1/3"), std::string::npos);
  EXPECT_NE(contents.find("graph.addNode: name must be non-empty"),
            std::string::npos);
  std::remove(log_path.c_str());

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// EVALUATE_MIXER(7) interception → bounded last_report (ORC-3)
// ---------------------------------------------------------------------------

TEST(QueueRunner, EvaluateReportStoredAndBounded) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);

  // Build a small src→out chain synchronously (runner not started: IDLE).
  sf_cmd_t nodes[2] = {make_add("src", SF_NODE_SOURCE),
                       make_add("out", SF_NODE_OUTPUT)};
  size_t applied = 0;
  char err[128] = "";
  ASSERT_EQ(sf_graph_apply_batch(p, nodes, 2, &applied, err, sizeof(err)), SF_OK);
  EXPECT_EQ(applied, 2u);
  const sfcore::SignalNode* srcn = find_by_name(ip, "src");
  const sfcore::SignalNode* outn = find_by_name(ip, "out");
  ASSERT_NE(srcn, nullptr);
  ASSERT_NE(outn, nullptr);
  sf_cmd_t edge = make_cmd(SF_CMD_ADD_EDGE);
  std::strncpy(edge.id1, srcn->id.c_str(), sizeof(edge.id1) - 1);
  std::strncpy(edge.id2, outn->id.c_str(), sizeof(edge.id2) - 1);
  ASSERT_EQ(sf_graph_apply_batch(p, &edge, 1, &applied, err, sizeof(err)), SF_OK);

  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  sf_cmd_t ev = make_cmd(SF_CMD_EVALUATE_MIXER);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &ev), SF_OK);

  char buf[8192 + 1];
  ASSERT_TRUE(wait_until(
      [&] { return sf_queue_runner_last_report(r, buf, sizeof(buf)) == SF_OK; },
      30000));

  // Small graph → the full report fits: every key present, no sentinel.
  const size_t report_len = std::strlen(buf);
  EXPECT_LE(report_len, 8192u);
  sfcore::json j = sfcore::json::parse(buf);
  EXPECT_TRUE(j.is_object());
  EXPECT_FALSE(j.contains("_truncated"));
  EXPECT_TRUE(j.contains("muted"));
  EXPECT_TRUE(j.contains("order"));
  EXPECT_EQ(j["order"].size(), 2u);
  EXPECT_TRUE(j.contains("outputs"));
  ASSERT_TRUE(j["outputs"].is_array());
  EXPECT_EQ(j["outputs"].size(), 1u);
  EXPECT_TRUE(j.contains("soloed"));

  // Bounded copy: a too-small caller buffer is SF_E_NOMEM, not a truncate.
  char tiny[8];
  EXPECT_EQ(sf_queue_runner_last_report(r, tiny, sizeof(tiny)), SF_E_NOMEM);
  EXPECT_STREQ(sf_last_error(nullptr), "runner.lastReport: buffer too small");

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(QueueRunner, EvaluateReportTruncatedSentinel) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);

  // 1500 isolated source nodes: the "order" array alone overflows 8 KiB.
  constexpr int kN = 1500;
  ip->doc.signalGraph.nodes.reserve(static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i) {
    sfcore::SignalNode n;
    n.id = sfcore::uuid_generate();
    n.kind = sfcore::SfNodeSource;
    n.name = "src" + std::to_string(i);
    ip->doc.signalGraph.nodes.push_back(n);
  }
  // Sanity: the full evaluation dump really would overflow the store cap.
  const size_t full_size = sfcore::evaluate_mixer(ip->doc.signalGraph).dump(2).size();
  EXPECT_GT(full_size, 8192u);

  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  sf_cmd_t ev = make_cmd(SF_CMD_EVALUATE_MIXER);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &ev), SF_OK);

  char buf[8192 + 1];
  ASSERT_TRUE(wait_until(
      [&] { return sf_queue_runner_last_report(r, buf, sizeof(buf)) == SF_OK; },
      30000));

  // ORC-3: stored report is ≤ 8 KiB content, valid JSON, with the sentinel.
  const size_t report_len = std::strlen(buf);
  EXPECT_LE(report_len, 8192u);
  EXPECT_NE(std::string(buf).find("\"_truncated\": true"), std::string::npos);
  sfcore::json j = sfcore::json::parse(buf);  // must never be invalid JSON
  EXPECT_TRUE(j.is_object());
  EXPECT_TRUE(j.value("_truncated", false));

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}