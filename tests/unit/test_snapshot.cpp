// SoundForge G4 P3 — PlanSnapshotStore + runner publish hook tests
// (PLAN_G4 §6 P3, §7.1 test_snapshot.cpp row; D1 4-slot protocol + R-A).
//
// Covers: publish into EMPTY (seq monotone); reclaim the oldest READY when no
// EMPTY slot remains; the reader acquires the newest READY and keeps it across
// "ticks"; release-then-reclaim; reader with no READY keeps the previous plan;
// first-ever tick renders silence (nullptr); compile-failure (cycle + simulated
// OOM/throw) restores the slot to EMPTY and leaves the last valid plan current;
// the STRESS loop (writer publishes N while the reader repeatedly acquires —
// no torn plan, no lost newest, writer never spins).
//
// Runner integration: a mutation enqueued while the runner is started fires the
// observer AFTER the apply and a fresh plan reflects the mutated graph; an empty
// drain and SF_CMD_STOP / cmd 7 (EVALUATE) do NOT republish; the observer is
// detached cleanly before destroy; the runner never writes proj->lastError
// (SEC-G3-4 unchanged).
#include <gtest/gtest.h>

#include "graph_internal.hpp"
#include "render_plan.hpp"
#include "sf_internal.hpp"
#include "snapshot.hpp"
#include "soundforge/sf_command_queue.h"
#include "soundforge/sf_graph.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_queue_runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

sfcore::SignalNode mk_node(const std::string& id, int kind, double gain_db = 0.0) {
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

sfcore::SignalGraphDoc source_to_output(const std::string& out_id, double gain_db) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node("src", sfcore::SfNodeSource, gain_db));
  g.nodes.push_back(mk_node("out", sfcore::SfNodeOutput, 0.0));
  g.edges.push_back(mk_edge("src", "out"));
  (void)out_id;
  return g;
}

// A graph whose one node is named `name` — proves "the plan reflects the
// mutated graph" independent of topology.
sfcore::SignalGraphDoc single_source(const std::string& name) {
  sfcore::SignalGraphDoc g;
  g.nodes.push_back(mk_node(name, sfcore::SfNodeSource));
  return g;
}

// Test-only compile hooks (the store's CompileFn seam). The store documents
// that the default (nullptr) is the real dsp::compile_render_plan.
bool throwing_compile(const sfcore::SignalGraphDoc&, const std::string&,
                      sfcore::dsp::RenderPlan&, std::string&) {
  throw std::bad_alloc();
}

bool runtime_error_compile(const sfcore::SignalGraphDoc&, const std::string&,
                           sfcore::dsp::RenderPlan&, std::string&) {
  throw std::runtime_error("injected");
}

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

// Test observer: records every publish it sees (including the graph node ids)
// and can simulate a slow/observing engine.
struct RecordingObserver {
  std::atomic<int> publishes{0};
  std::atomic<uint64_t> last_node_count{0};
  std::string last_first_node_id;
  std::mutex mu;

  static void fire(void* user, const sfcore::SignalGraphDoc& g) {
    auto* self = static_cast<RecordingObserver*>(user);
    self->publishes.fetch_add(1, std::memory_order_relaxed);
    self->last_node_count.store(g.nodes.size(), std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(self->mu);
    self->last_first_node_id = g.nodes.empty() ? std::string() : g.nodes.front().name;
  }

  sfcore::RunnerObserver as_runner_observer() {
    sfcore::RunnerObserver o;
    o.user = this;
    o.publish = &RecordingObserver::fire;
    return o;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Store protocol (D1)
// ---------------------------------------------------------------------------

TEST(PlanSnapshotStore, PublishIntoEmptyAssignsMonotoneSeq) {
  sfcore::PlanSnapshotStore store;
  std::string err;
  const sfcore::SignalGraphDoc g = source_to_output("out", -6.0);

  ASSERT_TRUE(store.publish(g, "out", err)) << err;
  EXPECT_EQ(store.published_seq(), 1u);
  EXPECT_EQ(store.slot_state(0), sfcore::PlanSnapshotStore::kSlotReady);

  // The store never publishes into a READING slot, so with nothing held the
  // next publish may reuse the just-freed/reclaimed slot.
  ASSERT_TRUE(store.publish(g, "out", err)) << err;
  EXPECT_EQ(store.published_seq(), 2u);
  ASSERT_TRUE(store.publish(g, "out", err)) << err;
  EXPECT_EQ(store.published_seq(), 3u);

  // seq monotone across four publishes (the 4th reuses a slot, never resets).
  ASSERT_TRUE(store.publish(g, "out", err)) << err;
  EXPECT_EQ(store.published_seq(), 4u);

  int ready = 0;
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    if (store.slot_state(i) == sfcore::PlanSnapshotStore::kSlotReady) ++ready;
  }
  EXPECT_LE(ready, sfcore::PlanSnapshotStore::kSlotCount);
  EXPECT_EQ(store.spin_count(), 0u);
}

TEST(PlanSnapshotStore, FirstEverTickWithNoPublishReturnsNull) {
  sfcore::PlanSnapshotStore store;
  uint64_t seq = 123;
  EXPECT_EQ(store.acquire(&seq), nullptr);  // silence is allowed
  EXPECT_EQ(seq, 0u);
  EXPECT_EQ(store.held_slot(), -1);
}

TEST(PlanSnapshotStore, ReclaimsOldestReadyWhenNoEmptySlotRemains) {
  sfcore::PlanSnapshotStore store;
  std::string err;
  const sfcore::SignalGraphDoc g = source_to_output("out", 0.0);

  // Fill every slot with a READY plan without the reader holding any (the
  // writer only ever needs ONE claimable slot at a time).
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    ASSERT_TRUE(store.publish(g, "out", err)) << err;
  }
  int ready = 0;
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    if (store.slot_state(i) == sfcore::PlanSnapshotStore::kSlotReady) ++ready;
  }
  ASSERT_EQ(ready, sfcore::PlanSnapshotStore::kSlotCount);  // no EMPTY left

  // No EMPTY slot remains -> the writer reclaims the READY with the SMALLEST
  // seq (the oldest), never spins.
  ASSERT_TRUE(store.publish(g, "out", err)) << err;
  EXPECT_EQ(store.published_seq(),
            static_cast<uint64_t>(sfcore::PlanSnapshotStore::kSlotCount) + 1);
  EXPECT_EQ(store.spin_count(), 0u);
  EXPECT_EQ(store.cas_failures(), 0u);

  // The four slot seqs must be {2,3,4,5}: the 5th publish reclaimed the oldest
  // slot (seq 1) and republished it as seq 5, so seq 1 is gone from the store.
  // (The old tautological EXPECT_GE(slot_seq(0), 0) proved nothing.)
  std::vector<uint64_t> slot_seqs;
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    slot_seqs.push_back(store.slot_seq(i));
  }
  std::sort(slot_seqs.begin(), slot_seqs.end());
  const std::vector<uint64_t> kExpectedSeqs = {2, 3, 4, 5};
  EXPECT_EQ(slot_seqs, kExpectedSeqs);
  EXPECT_EQ(store.published_seq(), 5u);
}

TEST(PlanSnapshotStore, ReaderAcquiresNewestAndKeepsItAcrossTicks) {
  sfcore::PlanSnapshotStore store;
  std::string err;
  const sfcore::SignalGraphDoc g1 = single_source("one");
  const sfcore::SignalGraphDoc g2 = single_source("two");

  ASSERT_TRUE(store.publish(g1, "", err)) << err;  // seq 1
  ASSERT_TRUE(store.publish(g2, "", err)) << err;  // seq 2

  uint64_t seq = 0;
  sfcore::dsp::RenderPlan* p = store.acquire(&seq);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(seq, 2u);  // newest
  ASSERT_EQ(p->node_index.size(), 1u);
  EXPECT_EQ(p->node_index[0].first, "two");

  // No new publish → the next tick keeps the SAME plan (no gap, no null), even
  // though the older seq-1 READY slot is still scannable.
  sfcore::dsp::RenderPlan* again = store.acquire(&seq);
  EXPECT_EQ(again, p);
  EXPECT_EQ(seq, 2u);

  // A newer publish replaces the held plan on the next acquire.
  ASSERT_TRUE(store.publish(single_source("three"), "", err)) << err;  // seq 3
  sfcore::dsp::RenderPlan* third = store.acquire(&seq);
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(seq, 3u);
  EXPECT_EQ(third->node_index[0].first, "three");
}

TEST(PlanSnapshotStore, ReaderKeepsPreviousPlanWhenNoReady) {
  sfcore::PlanSnapshotStore store;
  std::string err;
  const sfcore::SignalGraphDoc g = single_source("held");
  ASSERT_TRUE(store.publish(g, "", err)) << err;

  uint64_t seq = 0;
  sfcore::dsp::RenderPlan* p = store.acquire(&seq);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(seq, 1u);

  // A tick with no new publish keeps the previously-held plan (covered above);
  // releasing the hold and then acquiring with nothing READY returns nullptr
  // (the caller renders silence).
  store.release();
  EXPECT_EQ(store.held_slot(), -1);
  EXPECT_EQ(store.acquire(&seq), nullptr);
  EXPECT_EQ(seq, 0u);
}

TEST(PlanSnapshotStore, ReleaseThenReclaim) {
  sfcore::PlanSnapshotStore store;
  std::string err;
  ASSERT_TRUE(store.publish(single_source("a"), "", err)) << err;

  sfcore::dsp::RenderPlan* p = store.acquire(nullptr);
  ASSERT_NE(p, nullptr);
  const int held = store.held_slot();
  ASSERT_GE(held, 0);
  EXPECT_EQ(store.slot_state(held), sfcore::PlanSnapshotStore::kSlotReading);

  store.release();
  EXPECT_EQ(store.slot_state(held), sfcore::PlanSnapshotStore::kSlotEmpty);

  // The freed slot is claimable again for the next publish.
  ASSERT_TRUE(store.publish(single_source("b"), "", err)) << err;
  EXPECT_EQ(store.published_seq(), 2u);
  EXPECT_EQ(store.spin_count(), 0u);
}

TEST(PlanSnapshotStore, CompileCycleFailureRestoresEmptyAndKeepsLastValidPlan) {
  sfcore::PlanSnapshotStore store;
  std::string err;

  // Publish a valid plan first (the "last valid plan" the reader keeps).
  ASSERT_TRUE(store.publish(source_to_output("out", -6.0), "out", err)) << err;
  uint64_t seq = 0;
  sfcore::dsp::RenderPlan* before = store.acquire(&seq);
  ASSERT_NE(before, nullptr);
  EXPECT_EQ(seq, 1u);
  ASSERT_EQ(before->nodes.size(), 2u);

  // A cyclic graph: compile_render_plan fails via topological_order.
  sfcore::SignalGraphDoc cyc;
  cyc.nodes.push_back(mk_node("a", sfcore::SfNodeProcessor));
  cyc.nodes.push_back(mk_node("b", sfcore::SfNodeProcessor));
  cyc.edges.push_back(mk_edge("a", "b"));
  cyc.edges.push_back(mk_edge("b", "a"));

  EXPECT_FALSE(store.publish(cyc, "out", err));
  EXPECT_FALSE(err.empty());
  EXPECT_EQ(store.published_seq(), 1u);  // nothing new published

  // No slot left WRITING (the failure restored EMPTY).
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    EXPECT_NE(store.slot_state(i), sfcore::PlanSnapshotStore::kSlotWriting);
  }

  // The reader still holds the last valid plan: no null overwrite, no gap.
  uint64_t seq2 = 0;
  sfcore::dsp::RenderPlan* after = store.acquire(&seq2);
  EXPECT_EQ(after, before);
  EXPECT_EQ(seq2, 1u);
  ASSERT_EQ(after->nodes.size(), 2u);
  EXPECT_EQ(store.spin_count(), 0u);
}

TEST(PlanSnapshotStore, CompileOomAndExceptionFailuresRestoreEmpty) {
  sfcore::PlanSnapshotStore store;
  std::string err;
  ASSERT_TRUE(store.publish(single_source("valid"), "", err)) << err;

  // Simulated OOM (ORC-G4-03): a throwing compile hook. bad_alloc maps to the
  // out-of-memory error text and restores the slot to EMPTY.
  EXPECT_FALSE(store.publish(single_source("oom"), "", err, &throwing_compile));
  EXPECT_EQ(err, "render_plan: out of memory");
  EXPECT_EQ(store.published_seq(), 1u);
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    EXPECT_NE(store.slot_state(i), sfcore::PlanSnapshotStore::kSlotWriting);
  }

  // Arbitrary std::exception is contained the same way. ORC-G4P3-01: the
  // message is a static literal — the handler never allocates (no
  // concatenation of e.what()), so formatting can never throw before the slot
  // is restored to EMPTY.
  EXPECT_FALSE(store.publish(single_source("boom"), "", err, &runtime_error_compile));
  EXPECT_EQ(err, "render_plan: compile exception");
  EXPECT_EQ(store.published_seq(), 1u);
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    const int st = store.slot_state(i);
    EXPECT_TRUE(st == sfcore::PlanSnapshotStore::kSlotEmpty ||
                st == sfcore::PlanSnapshotStore::kSlotReady)
        << "slot " << i << " in state " << st << " after compile failures";
  }

  // The last valid plan is still current.
  uint64_t seq = 0;
  sfcore::dsp::RenderPlan* p = store.acquire(&seq);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(seq, 1u);
  EXPECT_EQ(store.spin_count(), 0u);
}

TEST(PlanSnapshotStore, CompileOomExhaustionNeverStrandsWritingSlot) {
  // ORC-G4P3-01 pin: a WRITING leak survives only if the failure path formats
  // the error text BEFORE restoring the slot (a string assignment/concat can
  // throw bad_alloc under genuine OOM). Four consecutive bad_alloc publishes
  // exercise exactly the exhaustion that would strand 4 WRITING slots and kill
  // the store; with the fix, each failure restores EMPTY first, so the store
  // stays fully claimable and publishing keeps working.
  sfcore::PlanSnapshotStore store;
  std::string err;

  // A valid plan first: a healthy baseline (slot 0 READY, seq 1) against which
  // the failure pins are checked.
  ASSERT_TRUE(store.publish(single_source("valid"), "", err)) << err;
  EXPECT_EQ(store.published_seq(), 1u);

  for (int i = 0; i < 4; ++i) {
    EXPECT_FALSE(store.publish(single_source("oom"), "", err, &throwing_compile))
        << "OOM publish #" << i << " unexpectedly succeeded";
    EXPECT_EQ(err, "render_plan: out of memory") << "OOM publish #" << i;
    for (int s = 0; s < sfcore::PlanSnapshotStore::kSlotCount; ++s) {
      EXPECT_NE(store.slot_state(s), sfcore::PlanSnapshotStore::kSlotWriting)
          << "slot " << s << " stranded WRITING after OOM publish #" << i;
    }
  }
  // No WRITING leak and no claim deadlock after the exhaustion loop.
  EXPECT_EQ(store.spin_count(), 0u);
  EXPECT_EQ(store.cas_failures(), 0u);
  EXPECT_EQ(store.published_seq(), 1u);  // nothing new was published

  // The store is still fully functional: a real publish succeeds (claims the
  // restored EMPTY slot), and every slot is EMPTY or READY.
  ASSERT_TRUE(store.publish(single_source("ok"), "", err)) << err;
  EXPECT_EQ(store.published_seq(), 2u);
  int ready = 0;
  for (int s = 0; s < sfcore::PlanSnapshotStore::kSlotCount; ++s) {
    const int st = store.slot_state(s);
    EXPECT_TRUE(st == sfcore::PlanSnapshotStore::kSlotEmpty ||
                st == sfcore::PlanSnapshotStore::kSlotReady)
        << "slot " << s << " in state " << st;
    if (st == sfcore::PlanSnapshotStore::kSlotReady) ++ready;
  }
  EXPECT_GE(ready, 2);  // the valid plan + the fresh publish
  EXPECT_EQ(store.spin_count(), 0u);

  // The fresh plan is the real (mutated) one — readable through the normal
  // reader path.
  uint64_t seq = 0;
  sfcore::dsp::RenderPlan* p = store.acquire(&seq);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(seq, 2u);
  ASSERT_EQ(p->node_index.size(), 1u);
  EXPECT_EQ(p->node_index[0].first, "ok");
}

TEST(PlanSnapshotStore, StressWriterReaderNoTornPlanNoLostNewestNoSpin) {
  // Writer publishes N distinct plans (each with a unique node id); the reader
  // repeatedly acquires. Assertions:
  //   * every plan the reader sees is fully compiled (node id matches the seq's
  //     expected id suffix) — no torn/partial plan;
  //   * the reader eventually observes the NEWEST published seq (never lost);
  //   * the writer never spun (spin_count == 0) — the 4-slot guarantee.
  constexpr int kPlans = 20000;
  sfcore::PlanSnapshotStore store;
  std::atomic<bool> writer_done{false};
  std::string writer_err;
  bool writer_ok = true;

  std::thread writer([&] {
    for (int i = 0; i < kPlans; ++i) {
      const std::string id = "n" + std::to_string(i);
      const sfcore::SignalGraphDoc g = single_source(id);
      std::string err;
      if (!store.publish(g, "", err)) {
        writer_ok = false;
        writer_err = err;
        break;
      }
    }
    writer_done.store(true, std::memory_order_release);
  });

  uint64_t best_seen = 0;
  std::vector<uint64_t> seen;
  seen.reserve(4096);
  bool torn = false;
  uint64_t torn_seq = 0;
  while (!writer_done.load(std::memory_order_acquire)) {
    uint64_t seq = 0;
    sfcore::dsp::RenderPlan* p = store.acquire(&seq);
    if (!p) continue;  // first ever tick: silence permitted, then proceed
    if (p->node_index.size() != 1u) {
      torn = true;
      torn_seq = seq;
      break;
    }
    const std::string expect = "n" + std::to_string(seq - 1);
    if (p->node_index[0].first != expect) {
      torn = true;
      torn_seq = seq;
      break;
    }
    best_seen = std::max(best_seen, seq);
    seen.push_back(seq);
  }
  writer.join();
  ASSERT_TRUE(writer_ok) << writer_err;
  EXPECT_FALSE(torn) << "torn/partial plan observed at seq " << torn_seq;
  EXPECT_FALSE(seen.empty());
  EXPECT_EQ(store.spin_count(), 0u) << "writer had to wait for the reader";

  // Reading the newest requires the reader to have caught up. Acquire until it
  // sees kPlans (the writer is done; no more publishes can race).
  for (int i = 0; i < 100000 && best_seen < static_cast<uint64_t>(kPlans); ++i) {
    uint64_t seq = 0;
    sfcore::dsp::RenderPlan* p = store.acquire(&seq);
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(p->node_index.size(), 1u);
    EXPECT_EQ(p->node_index[0].first, "n" + std::to_string(seq - 1))
        << "torn plan at seq " << seq;
    best_seen = std::max(best_seen, seq);
  }
  EXPECT_EQ(best_seen, static_cast<uint64_t>(kPlans));
  EXPECT_EQ(store.published_seq(), static_cast<uint64_t>(kPlans));

  // After release, no slot is left READING (no leak of a READING slot).
  store.release();
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    EXPECT_NE(store.slot_state(i), sfcore::PlanSnapshotStore::kSlotReading);
  }
}

// ---------------------------------------------------------------------------
// Runner integration (PLAN_G4 §6 P3)
// ---------------------------------------------------------------------------

TEST(SnapshotRunner, ObserverFiresAfterAppliedBatchWithMutatedGraph) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  RecordingObserver obs;
  const sfcore::RunnerObserver ro = obs.as_runner_observer();
  sfcore::sf_queue_runner_set_observer(r, &ro);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  sf_cmd_t add = make_add("live", SF_NODE_SOURCE);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &add), SF_OK);
  ASSERT_TRUE(wait_until([&] { return obs.publishes.load() >= 1; }, 30000));
  EXPECT_EQ(obs.last_node_count.load(), 1u);
  {
    std::lock_guard<std::mutex> lk(obs.mu);
    EXPECT_EQ(obs.last_first_node_id, "live");
  }

  // A second mutation fires a second publish with the grown graph.
  sf_cmd_t add2 = make_add("live2", SF_NODE_OUTPUT);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &add2), SF_OK);
  ASSERT_TRUE(wait_until([&] { return obs.publishes.load() >= 2; }, 30000));
  EXPECT_EQ(obs.last_node_count.load(), 2u);

  // SEC-G3-4: the runner never writes the handle error store.
  EXPECT_STREQ(sf_last_error(p), "no error");

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);
  sfcore::sf_queue_runner_set_observer(r, nullptr);  // detach before destroy
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(SnapshotRunner, EmptyDrainStopAndEvaluateDoNotPublish) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  RecordingObserver obs;
  const sfcore::RunnerObserver ro = obs.as_runner_observer();
  sfcore::sf_queue_runner_set_observer(r, &ro);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  // Idle the runner for a bit: empty drains must not publish.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(obs.publishes.load(), 0);

  // EVALUATE_MIXER (cmd 7) is intercepted — never applied, never published.
  sf_cmd_t ev = make_cmd(SF_CMD_EVALUATE_MIXER);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &ev), SF_OK);
  ASSERT_TRUE(wait_until(
      [&] {
        char buf[256];
        return sf_queue_runner_last_report(r, buf, sizeof(buf)) == SF_OK;
      },
      30000));
  EXPECT_EQ(obs.publishes.load(), 0);

  // SF_CMD_STOP is a barrier, not a mutation: no publish.
  sf_cmd_t stop = make_cmd(SF_CMD_STOP);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &stop), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);  // exits on the barrier
  EXPECT_EQ(obs.publishes.load(), 0);

  sfcore::sf_queue_runner_set_observer(r, nullptr);
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(SnapshotRunner, DetachBeforeDestroyStopsPublishing) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  RecordingObserver obs;
  const sfcore::RunnerObserver ro = obs.as_runner_observer();
  sfcore::sf_queue_runner_set_observer(r, &ro);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  sf_cmd_t add = make_add("n", SF_NODE_SOURCE);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &add), SF_OK);
  ASSERT_TRUE(wait_until([&] { return obs.publishes.load() >= 1; }, 30000));

  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);

  // Post-join detach: further (no) publishes cannot reach the observer, and
  // the store/observer lifetime is no longer referenced by the runner.
  const int before = obs.publishes.load();
  sfcore::sf_queue_runner_set_observer(r, nullptr);
  EXPECT_EQ(obs.publishes.load(), before);

  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

TEST(SnapshotRunner, PlanSnapshotStoreAsObserverReflectsMutatedGraph) {
  // ORC-G4P3-02: the runner's publish observer must be able to be the REAL
  // PlanSnapshotStore (the D1 wiring the P4 engine performs) — the existing
  // runner tests only attach RecordingObserver (node-id recording), never the
  // store. Wire the store in before start, enqueue a mutation while the runner
  // is started, then drain-synchronize (wait for a publish, then stop+join —
  // deterministic, no fixed sleeps) and verify the store through the same
  // introspection API the store tests use.
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_queue_runner_t* r = nullptr;
  ASSERT_EQ(sf_queue_runner_create(&r, q, p), SF_OK);

  sfcore::PlanSnapshotStore store;
  sfcore::RunnerObserver ro;
  ro.user = &store;
  ro.publish = [](void* user, const sfcore::SignalGraphDoc& g) {
    auto* st = static_cast<sfcore::PlanSnapshotStore*>(user);
    std::string err;
    (void)st->publish(g, /*out_node_id=*/"", err);
  };
  sfcore::sf_queue_runner_set_observer(r, &ro);
  ASSERT_EQ(sf_queue_runner_start(r), SF_OK);

  // Enqueue a mutation while the runner is started. Whether the runner drains
  // both commands in one batch (one publish after the batch) or in two passes
  // (two publishes), the NEWEST READY plan always reflects the fully-mutated
  // graph.
  sf_cmd_t add = make_add("live", SF_NODE_SOURCE);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &add), SF_OK);
  sf_cmd_t add2 = make_add("live2", SF_NODE_OUTPUT);
  ASSERT_EQ(sf_cmd_queue_enqueue(q, &add2), SF_OK);

  // Drain sync (the existing runner-test pattern): wait for the observer to
  // fire at least once (published_seq > 0), then stop+join so the store is
  // fully quiescent before introspection — no live writer can race the loads.
  ASSERT_TRUE(wait_until([&] { return store.published_seq() > 0; }, 30000));
  ASSERT_EQ(sf_queue_runner_stop(r), SF_OK);
  ASSERT_EQ(sf_queue_runner_join(r), SF_OK);

  // (c) The published seq advanced: the runner published >=1 plan.
  const uint64_t ps = store.published_seq();
  EXPECT_GE(ps, 1u);

  // (a) At least one slot holds a READY plan.
  int ready_slot = -1;
  for (int i = 0; i < sfcore::PlanSnapshotStore::kSlotCount; ++i) {
    if (store.slot_state(i) == sfcore::PlanSnapshotStore::kSlotReady) {
      ready_slot = i;
      break;
    }
  }
  ASSERT_GE(ready_slot, 0) << "no READY slot after the runner drained";
  EXPECT_EQ(store.spin_count(), 0u);

  // (b) The plan in the store reflects the MUTATED graph: acquire the newest
  // READY plan (the introspection API the store tests use) and inspect it. The
  // runner-generated node ids are UUIDs (ADD_NODE: cmd.id1 becomes the node
  // NAME), so identity is pinned via the kinds of the two added nodes — one
  // source (cmd 1) and one output (cmd 2) — plus the 2-node counts.
  uint64_t seq = 0;
  sfcore::dsp::RenderPlan* plan = store.acquire(&seq);
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(seq, ps);  // acquired the newest published plan
  ASSERT_EQ(plan->node_index.size(), 2u);  // both mutations are in the plan
  ASSERT_EQ(plan->nodes.size(), 2u);
  const bool have_source =
      plan->nodes[0].kind == sfcore::SfNodeSource ||
      plan->nodes[1].kind == sfcore::SfNodeSource;
  const bool have_output =
      plan->nodes[0].kind == sfcore::SfNodeOutput ||
      plan->nodes[1].kind == sfcore::SfNodeOutput;
  EXPECT_TRUE(have_source) << "mutated source node missing from published plan";
  EXPECT_TRUE(have_output) << "mutated output node missing from published plan";
  store.release();

  // SEC-G3-4: the runner never wrote the handle error store.
  EXPECT_STREQ(sf_last_error(p), "no error");

  sfcore::sf_queue_runner_set_observer(r, nullptr);  // detach before destroy
  ASSERT_EQ(sf_queue_runner_destroy(r), SF_OK);
  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}
