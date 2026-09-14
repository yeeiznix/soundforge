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

#include <atomic>
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
// P4b single-owner guard sweep (PLAN_G3 §6 P4b): every doc-touching ABI entry
// rejects while the runner owns the mutation thread. Mutators/reads return
// SF_E_IO "project.busy: queue runner active"; destroy has its own void-safe
// rule; sf_last_error is exempt.
// ---------------------------------------------------------------------------

constexpr const char* kBusy = "project.busy: queue runner active";

// A running runner with a live producer keeping the queue non-empty, so the
// guard is exercised mid-drain. Returns after start; caller stops+joins.
struct DrainFixture {
  sf_project_t* p = nullptr;
  sf_cmd_queue_t* q = nullptr;
  sf_queue_runner_t* r = nullptr;
  std::thread producer;
  std::atomic<bool> run_producer{true};

  bool start() {
    p = sf_project_create("B", nullptr);
    if (!p) return false;
    if (sf_cmd_queue_create(&q) != SF_OK) return false;
    if (sf_queue_runner_create(&r, q, p) != SF_OK) return false;
    if (sf_queue_runner_start(r) != SF_OK) return false;
    producer = std::thread([this] {
      int i = 0;
      while (run_producer.load()) {
        char name[16];
        std::snprintf(name, sizeof(name), "p%06d", i++);
        sf_cmd_t c = make_add(name, SF_NODE_PROCESSOR);
        if (sf_cmd_queue_enqueue(q, &c) != SF_OK) std::this_thread::yield();
      }
    });
    // A steady producer means the runner is genuinely mid-drain.
    return wait_until([&] { return sf_cmd_queue_depth(q) > 0; }, 30000);
  }

  void stop_and_join() {
    run_producer = false;
    if (producer.joinable()) producer.join();
    sf_queue_runner_stop(r);
    sf_queue_runner_join(r);
  }

  void cleanup() {
    if (r) sf_queue_runner_destroy(r);
    if (q) sf_cmd_queue_destroy(q);
    if (p) sf_project_destroy(p);
  }
};

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

// ===========================================================================
// P4b — single-owner guard sweep (PLAN_G3 §6 P4b, D3-amd SEC-G3-2/-G3-4)
// ===========================================================================

// Hammer the doc-touching mutators while a runner is actively draining: every
// call must reject SF_E_IO + the verbatim busy string. After stop+join the same
// calls succeed (C8 hands ownership back). Covers the 13 non-apply_batch
// mutators; sf_graph_apply_batch is already guarded by P4a.
TEST(QueueRunner, P4bConcurrentMutatorHammerDuringDrain) {
  DrainFixture f;
  ASSERT_TRUE(f.start());
  auto* ip = reinterpret_cast<sfcore::SfProject*>(f.p);
  EXPECT_EQ(runner_state(ip), static_cast<int32_t>(sfcore::SfRunnerRunning));

  char out_id[37];
  char err[128];

  // (1) The plan's four hammered mutators, many iterations each.
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(sf_graph_add_node(f.p, SF_NODE_PROCESSOR, "hammer", out_id), SF_E_IO);
    EXPECT_STREQ(sf_last_error(f.p), kBusy);
    EXPECT_EQ(sf_graph_set_mixer(f.p, "bogus", 0.0, 0.0, 0, 0), SF_E_IO);
    EXPECT_STREQ(sf_last_error(f.p), kBusy);
    EXPECT_EQ(sf_project_save_to_path(f.p, "sf_guard_busy.json"), SF_E_IO);
    EXPECT_STREQ(sf_last_error(f.p), kBusy);
    EXPECT_EQ(sf_scene_rename(f.p, "hammer"), SF_E_IO);
    EXPECT_STREQ(sf_last_error(f.p), kBusy);
  }

  // (2) Every remaining mutator site, once each, to prove sweep completeness.
  EXPECT_EQ(sf_graph_remove_node(f.p, "bogus"), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  EXPECT_EQ(sf_graph_add_edge(f.p, "a", "b", 0, 0, out_id), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  EXPECT_EQ(sf_graph_remove_edge(f.p, "bogus"), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  EXPECT_EQ(sf_graph_set_preset(f.p, "bogus", "preset"), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  EXPECT_EQ(sf_project_rename(f.p, "n"), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  EXPECT_EQ(sf_venue_rename(f.p, "n"), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  EXPECT_EQ(sf_venue_set_dimensions(f.p, 1.0, 1.0, 1.0), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  EXPECT_EQ(sf_scene_set_geometry(f.p, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1), SF_E_IO);
  EXPECT_STREQ(sf_last_error(f.p), kBusy);
  sf_cmd_t batched = make_add("batched", SF_NODE_PROCESSOR);
  size_t applied = 99;
  EXPECT_EQ(sf_graph_apply_batch(f.p, &batched, 1, &applied, err, sizeof(err)), SF_E_IO);
  EXPECT_STREQ(err, kBusy);
  EXPECT_EQ(applied, 0u);

  // (3) stop+join → ownership returns; the SAME calls now succeed (C8).
  f.stop_and_join();

  // Busy rejects must not have touched the document: only the producer's
  // add_node commands applied. Sampled AFTER join so the vector read can never
  // race the runner thread (single-owner discipline the sweep enforces).
  const size_t during = ip->doc.signalGraph.nodes.size();
  EXPECT_GT(during, 0u);
  EXPECT_EQ(sf_graph_add_node(f.p, SF_NODE_PROCESSOR, "post", out_id), SF_OK);
  EXPECT_EQ(sf_graph_set_mixer(f.p, out_id, -3.0, 0.0, 0, 0), SF_OK);
  EXPECT_EQ(sf_project_save_to_path(f.p, "sf_guard_ok.json"), SF_OK);
  EXPECT_EQ(sf_scene_rename(f.p, "post rename"), SF_OK);
  EXPECT_EQ(sf_project_rename(f.p, "post project"), SF_OK);
  EXPECT_EQ(sf_venue_rename(f.p, "post venue"), SF_OK);
  EXPECT_EQ(sf_venue_set_dimensions(f.p, 5.0, 6.0, 3.0), SF_OK);
  EXPECT_EQ(sf_scene_set_geometry(f.p, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1), SF_OK);
  std::remove("sf_guard_ok.json");
  ASSERT_FALSE(std::ifstream("sf_guard_busy.json").good());  // rejected: no file

  f.cleanup();
}

// Every doc-touching read rejects while RUNNING; sf_last_error(p) is exempt
// (the runner never writes lastError). After join the reads succeed.
TEST(QueueRunner, P4bReadRejectDuringRunning) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  // Seed one node pre-start so the graph reads have something to say.
  char out_id[37];
  ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s", out_id), SF_OK);

  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  char* js = nullptr;
  size_t jlen = 0;
  char report[4096];
  char* order = nullptr;
  size_t order_len = 0;
  sf_project_t* clone = nullptr;

  EXPECT_EQ(sf_graph_evaluate_mixer(p, &js, &jlen), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(js, nullptr);
  EXPECT_EQ(sf_graph_validate(p, report, sizeof(report)), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(sf_graph_topological_order(p, &order, &order_len), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(order, nullptr);
  EXPECT_EQ(sf_project_to_json(p, &js, &jlen), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(js, nullptr);
  EXPECT_EQ(sf_project_health_check(p, report, sizeof(report)), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(sf_project_clone(p, &clone), SF_E_IO);  // ORC-1: RUNNING clone rejects
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(clone, nullptr);

  // Getters: safe empty value + thread-local busy error.
  EXPECT_STREQ(sf_project_get_name(p), "");
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_STREQ(sf_project_get_id(p), "");
  EXPECT_EQ(sf_project_get_schema_version(p), -1);
  EXPECT_STREQ(sf_project_get_engine_version(p), "");

  // Exempt: sf_last_error(handle) still readable; runner never wrote it.
  EXPECT_STREQ(sf_last_error(p), "no error");

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);

  // Post-join (C8): reads succeed.
  EXPECT_EQ(sf_graph_evaluate_mixer(p, &js, &jlen), SF_OK);
  ASSERT_NE(js, nullptr);
  sf_free_string(js);
  EXPECT_EQ(sf_project_to_json(p, &js, &jlen), SF_OK);
  ASSERT_NE(js, nullptr);
  sf_free_string(js);
  EXPECT_EQ(sf_project_health_check(p, report, sizeof(report)), SF_OK);
  EXPECT_EQ(sf_project_clone(p, &clone), SF_OK);
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(sf_project_get_schema_version(p), SF_SCHEMA_VERSION);

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_project_destroy(clone);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// SEC-G3-2 (void-safe): destroy with a live runner skips the free, reports via
// the handle error store, and the handle stays usable. stop+join then a second
// destroy proceeds (mirrors P4a's real-thread pattern).
TEST(QueueRunner, P4bDestroyWithoutJoinRejectsThenSecondDestroyProceeds) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  // RUNNING: free is skipped. The handle survives (proven by a subsequent
  // read touching the same address WITHOUT ASan/UBSan reporting a UAF).
  sf_project_destroy(p);
  EXPECT_STREQ(sf_last_error(p), "project.destroy: queue runner active");
  EXPECT_EQ(sf_project_get_schema_version(p), -1);  // busy read, handle NOT freed

  // stop + join returns ownership (C8), and the runner handle is now
  // destroyable — destroy it BEFORE freeing the project it references,
  // honoring the runner→queue→project teardown order (no leak, no dangling
  // reference).
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  EXPECT_EQ(runner_state(ip), static_cast<int32_t>(sfcore::SfRunnerStopped));
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);

  // The mandatory SECOND destroy now proceeds (free succeeds). We do not touch
  // p afterwards — reading a freed handle is UB.
  sf_project_destroy(p);
  sf_cmd_queue_destroy(q);
}

// Deterministic STOPPING-window probe for the destroy guard. The real
// stop()->join window is a few microseconds (the epilogue publishes STOPPED as
// soon as the last batch drains), so racing it would be flaky; instead we drive
// the state directly (no thread: create leaves it IDLE) — the P4a-oracle
// accepted approach.
TEST(QueueRunner, P4bDestroyRejectedInStoppingWindow) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  ip->runnerState.store(static_cast<int32_t>(sfcore::SfRunnerStopping),
                        std::memory_order_release);

  // Destroy (void) must reject + skip the free; the handle is still alive.
  sf_project_destroy(p);
  EXPECT_STREQ(sf_last_error(p), "project.destroy: queue runner active");
  // A read also rejects — and never touches doc.
  char* js = nullptr;
  size_t jlen = 0;
  EXPECT_EQ(sf_project_to_json(p, &js, &jlen), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  // sf_last_error(handle) is the exempt probe proving the handle is intact.
  EXPECT_STREQ(sf_last_error(p), "project.destroy: queue runner active");

  // Runner destroy also rejects while STOPPING (handle not freed).
  EXPECT_EQ(sf_queue_runner_destroy(r), SF_E_IO);
  EXPECT_STREQ(sf_last_error(p), "project.destroy: queue runner active");
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);  // no thread was ever started

  // Simulate the runner epilogue + join: now STOPPED(joined) → both proceed.
  ip->runnerState.store(static_cast<int32_t>(sfcore::SfRunnerStopped),
                        std::memory_order_release);
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// ORC-1: clone in IDLE → runner state IDLE; clone while RUNNING → SF_E_IO.
TEST(QueueRunner, P4bCloneIdleIsIdleAndRunningRejects) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);

  // IDLE clone: state defaults to IDLE (copy ctor), even after a full
  // start/stop/join session on the source (STOPPED must not be inherited).
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  // RUNNING clone → rejected (read guard; ORC-1).
  sf_project_t* clone = nullptr;
  EXPECT_EQ(sf_project_clone(p, &clone), SF_E_IO);
  EXPECT_STREQ(sf_last_error(nullptr), kBusy);
  EXPECT_EQ(clone, nullptr);

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  ASSERT_EQ(runner_state(reinterpret_cast<sfcore::SfProject*>(p)),
            static_cast<int32_t>(sfcore::SfRunnerStopped));

  // IDLE clone of a STOPPED source: the copy ctor default-constructs runner
  // state, so the clone is IDLE (never inherits STOPPED).
  ASSERT_EQ(sf_project_clone(p, &clone), SF_OK);
  ASSERT_NE(clone, nullptr);
  EXPECT_EQ(runner_state(reinterpret_cast<sfcore::SfProject*>(clone)),
            static_cast<int32_t>(sfcore::SfRunnerIdle));
  // A clone is fully usable synchronously (it owns its own mutation thread).
  char out_id[37];
  EXPECT_EQ(sf_graph_add_node(clone, SF_NODE_SOURCE, "c", out_id), SF_OK);

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(clone);
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