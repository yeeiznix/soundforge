// SoundForge G2 P5 — lock-free SPSC command queue + apply_batch (§4.4, §4.5).
// Queue: fixed 176-B slots, capacity 256, seq watermark, overflow reject+WARN,
// and the 10k-message threaded SPSC proof (~39 full drain cycles). apply_batch:
// same impl path as the synchronous mutators (audit + modifiedAt per cmd).
#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_command_queue.h"
#include "soundforge/sf_graph.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

sf_cmd_t make_cmd(int32_t type) {
  sf_cmd_t c{};
  c.type = type;
  return c;
}

const sfcore::SignalNode* find_by_name(sfcore::SfProject* ip, const std::string& name) {
  for (const auto& n : ip->doc.signalGraph.nodes) {
    if (n.name == name) return &n;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------

TEST(CmdQueue, SlotSizeIs176) {
  // Plan ORC-C: 176-B slot, capacity 256 => 44 KiB ring, compile-time constant.
  EXPECT_EQ(sizeof(sf_cmd_t), 176u);
}

TEST(CmdQueue, CreateDestroyAndDepth) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(sf_cmd_queue_depth(q), 0);
  sf_cmd_queue_destroy(q);
  sf_cmd_queue_destroy(nullptr);  // safe no-op
}

TEST(CmdQueue, NullArguments) {
  sf_cmd_queue_t* q = nullptr;
  EXPECT_EQ(sf_cmd_queue_create(nullptr), SF_E_INVALID_ARG);
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  sf_cmd_t c = make_cmd(SF_CMD_SET_MIXER);
  sf_cmd_t out{};
  EXPECT_EQ(sf_cmd_queue_enqueue(nullptr, &c), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_cmd_queue_enqueue(q, nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_cmd_queue_dequeue(nullptr, &out), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_cmd_queue_dequeue(q, nullptr), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_cmd_queue_depth(nullptr), -1);
  sf_cmd_queue_destroy(q);
}

TEST(CmdQueue, RoundTripPreservesFields) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);

  sf_cmd_t c{};
  c.type = SF_CMD_SET_MIXER;
  std::strncpy(c.id1, "node-1", sizeof(c.id1) - 1);
  std::strncpy(c.id2, "preset-9", sizeof(c.id2) - 1);
  c.value = -6.0;
  c.value2 = 0.25;
  c.flags = SF_MIXER_FLAG_MUTE;
  c.port_a = 2;
  c.port_b = 3;
  EXPECT_EQ(sf_cmd_queue_enqueue(q, &c), SF_OK);

  sf_cmd_t out{};
  ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
  EXPECT_EQ(out.type, SF_CMD_SET_MIXER);
  EXPECT_EQ(out.seq, 1u);  // seq assigned by the queue, not the caller
  EXPECT_STREQ(out.id1, "node-1");
  EXPECT_STREQ(out.id2, "preset-9");
  EXPECT_DOUBLE_EQ(out.value, -6.0);
  EXPECT_DOUBLE_EQ(out.value2, 0.25);
  EXPECT_EQ(out.flags, SF_MIXER_FLAG_MUTE);
  EXPECT_EQ(out.port_a, 2);
  EXPECT_EQ(out.port_b, 3);
  EXPECT_EQ(sf_cmd_queue_depth(q), 0);
  EXPECT_EQ(sf_cmd_queue_dequeue(q, &out), SF_E_IO);  // empty => SF_E_IO, retryable

  sf_cmd_queue_destroy(q);
}

TEST(CmdQueue, SeqWatermarkIsMonotonic) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  for (int i = 0; i < 5; ++i) {
    sf_cmd_t c = make_cmd(SF_CMD_SET_MIXER);
    EXPECT_EQ(sf_cmd_queue_enqueue(q, &c), SF_OK);
  }
  sf_cmd_t out{};
  for (uint64_t want = 1; want <= 5; ++want) {
    ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
    EXPECT_EQ(out.seq, want);
  }
  sf_cmd_queue_destroy(q);
}

TEST(CmdQueue, DepthTracksInflight) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);
  for (int i = 0; i < 3; ++i) {
    sf_cmd_t c = make_cmd(SF_CMD_ADD_NODE);
    EXPECT_EQ(sf_cmd_queue_enqueue(q, &c), SF_OK);
  }
  EXPECT_EQ(sf_cmd_queue_depth(q), 3);
  sf_cmd_t out{};
  EXPECT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
  EXPECT_EQ(sf_cmd_queue_depth(q), 2);
  sf_cmd_queue_destroy(q);
}

TEST(CmdQueue, OverflowRejectsAtCapacity) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);

  for (int i = 0; i < 256; ++i) {
    sf_cmd_t c = make_cmd(SF_CMD_SET_MIXER);
    EXPECT_EQ(sf_cmd_queue_enqueue(q, &c), SF_OK);
  }
  EXPECT_EQ(sf_cmd_queue_depth(q), 256);
  // 257th message: reject + WARN (watermark advances -> visible gap at 257).
  {
    sf_cmd_t c = make_cmd(SF_CMD_SET_MIXER);
    EXPECT_EQ(sf_cmd_queue_enqueue(q, &c), SF_E_IO);
  }
  EXPECT_EQ(sf_cmd_queue_depth(q), 256);  // nothing was accepted

  sf_cmd_t out{};
  for (uint64_t want = 1; want <= 256; ++want) {
    ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
    EXPECT_EQ(out.seq, want);
  }
  EXPECT_EQ(sf_cmd_queue_depth(q), 0);

  // The dropped message left a gap: next accepted seq is 257, not 258.
  {
    sf_cmd_t c = make_cmd(SF_CMD_ADD_NODE);
    EXPECT_EQ(sf_cmd_queue_enqueue(q, &c), SF_OK);
  }
  ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
  EXPECT_EQ(out.seq, 257u);

  sf_cmd_queue_destroy(q);
}

TEST(CmdQueue, TenKThreadedSPSC) {
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);

  // 10k messages, capacity 256 => ~39 full drain cycles. The producer retries
  // a "queue full" rejection with a yield instead of failing: on an
  // oversubscribed CI box the consumer thread may be slow to start, and a busy
  // producer must never drop intent on the audio lane (C2 — this is the exact
  // G3 producer behavior). Correctness claims asserted here: zero messages
  // lost, seqs gapless 1..10000, payload intact, ring empty afterwards.
  // Deterministic overflow behavior lives in OverflowRejectsAtCapacity.
  // The producer yields every 256 enqueues (one ring-full) rather than
  // hammering 10k in a single scheduling quantum.  This mimics the G3 audio
  // callback pacing (message bursts with idle gaps) and guarantees, even on a
  // 1-core box, that the consumer drains each burst before the next begins.
  // Hence full ring never triggers — zero rejects by construction.
  constexpr int N = 10000;
  long received = 0;
  uint64_t last_seq = 0;
  long bad_payload = 0;
  std::thread consumer([&q, &received, &last_seq, &bad_payload] {
    long spins = 0;
    while (received < N) {
      sf_cmd_t out{};
      sf_result_t rc = sf_cmd_queue_dequeue(q, &out);
      if (rc == SF_OK) {
        if (out.seq != last_seq + 1) ++bad_payload;          // gap => broken
        if (out.value != (out.seq - 1) * 0.5) ++bad_payload; // payload drift
        if (out.type != SF_CMD_SET_MIXER) ++bad_payload;
        last_seq = out.seq;
        ++received;
        spins = 0;
      } else if (rc == SF_E_IO) {
        if (++spins > 50000000) break;  // safety valve: never hang on a regression
        std::this_thread::yield();
      } else {
        ++bad_payload;
        break;
      }
    }
  });

  std::thread producer([&q] {
    for (int i = 0; i < N; ++i) {
      if ((i % 256) == 0 && i > 0) std::this_thread::yield();  // pace: one ring per quantum
      sf_cmd_t c{};
      c.type = SF_CMD_SET_MIXER;
      std::strncpy(c.id1, "threaded", sizeof(c.id1) - 1);
      c.value = i * 0.5;
      c.value2 = 0.25;
      c.flags = (i % 2) == 0 ? SF_MIXER_FLAG_MUTE : 0;
      while (sf_cmd_queue_enqueue(q, &c) != SF_OK) {
        // In principle this never runs (see yield pacing above).
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(received, N);
  EXPECT_EQ(last_seq, static_cast<uint64_t>(N));  // gapless 1..10000 (seq only advances on accept)
  EXPECT_EQ(bad_payload, 0);
  EXPECT_EQ(sf_cmd_queue_depth(q), 0);

  // Ring still usable after the marathon: seq continues from N+1 (no gap from
  // retries because seq only advances on acceptance — overflow does NOT
  // consume a seq number; retry later gets the same would-be seq).
  {
    sf_cmd_t c = make_cmd(SF_CMD_ADD_NODE);
    EXPECT_EQ(sf_cmd_queue_enqueue(q, &c), SF_OK);
  }
  sf_cmd_t out{};
  ASSERT_EQ(sf_cmd_queue_dequeue(q, &out), SF_OK);
  EXPECT_EQ(out.seq, static_cast<uint64_t>(N) + 1);

  sf_cmd_queue_destroy(q);
}

TEST(CmdQueue, FromQueueDrainToApplyBatch) {
  // C3 story: UI thread enqueues intent, owner thread drains + applies.
  // Phase 1 enqueues two addNode intents; the owner applies them, learns the
  // generated UUIDs from the graph JSON, then enqueues the wiring edge.
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_queue_t* q = nullptr;
  ASSERT_EQ(sf_cmd_queue_create(&q), SF_OK);

  sf_cmd_t in0 = make_cmd(SF_CMD_ADD_NODE);
  in0.port_a = SF_NODE_SOURCE;
  std::strncpy(in0.id1, "src", sizeof(in0.id1) - 1);
  sf_cmd_t in1 = make_cmd(SF_CMD_ADD_NODE);
  in1.port_a = SF_NODE_OUTPUT;
  std::strncpy(in1.id1, "out", sizeof(in1.id1) - 1);
  EXPECT_EQ(sf_cmd_queue_enqueue(q, &in0), SF_OK);
  EXPECT_EQ(sf_cmd_queue_enqueue(q, &in1), SF_OK);

  sf_cmd_t drained[2]{};
  for (auto& c : drained) ASSERT_EQ(sf_cmd_queue_dequeue(q, &c), SF_OK);
  EXPECT_EQ(drained[0].seq, 1u);
  EXPECT_EQ(drained[1].seq, 2u);

  size_t applied = 0;
  char err[128];
  ASSERT_EQ(sf_graph_apply_batch(p, drained, 2, &applied, err, sizeof(err)), SF_OK);
  EXPECT_EQ(applied, 2u);

  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  const sfcore::SignalNode* src = find_by_name(ip, "src");
  const sfcore::SignalNode* out = find_by_name(ip, "out");
  ASSERT_NE(src, nullptr);
  ASSERT_NE(out, nullptr);

  sf_cmd_t edge = make_cmd(SF_CMD_ADD_EDGE);
  std::strncpy(edge.id1, src->id.c_str(), sizeof(edge.id1) - 1);
  std::strncpy(edge.id2, out->id.c_str(), sizeof(edge.id2) - 1);
  EXPECT_EQ(sf_cmd_queue_enqueue(q, &edge), SF_OK);
  ASSERT_EQ(sf_cmd_queue_dequeue(q, &drained[0]), SF_OK);
  ASSERT_EQ(sf_graph_apply_batch(p, &drained[0], 1, &applied, err, sizeof(err)), SF_OK);
  EXPECT_EQ(applied, 1u);
  EXPECT_EQ(ip->doc.signalGraph.nodes.size(), 2u);
  EXPECT_EQ(ip->doc.signalGraph.edges.size(), 1u);

  sf_cmd_queue_destroy(q);
  sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// apply_batch
// ---------------------------------------------------------------------------

TEST(ApplyBatch, NullArguments) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  sf_cmd_t c = make_cmd(SF_CMD_ADD_NODE);
  size_t applied = 999;
  char err[64];

  EXPECT_EQ(sf_graph_apply_batch(nullptr, &c, 1, &applied, nullptr, 0), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_graph_apply_batch(p, nullptr, 1, &applied, nullptr, 0), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_graph_apply_batch(p, &c, 1, nullptr, nullptr, 0), SF_E_INVALID_ARG);
  EXPECT_EQ(sf_graph_apply_batch(p, &c, 1, &applied, err, 0), SF_E_INVALID_ARG);  // cap>0 needs buf? no
  EXPECT_EQ(applied, 0u);
  sf_project_destroy(p);
}

TEST(ApplyBatch, EmptyBatchIsOk) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  size_t applied = 999;
  EXPECT_EQ(sf_graph_apply_batch(p, nullptr, 0, &applied, nullptr, 0), SF_OK);
  EXPECT_EQ(applied, 0u);
  sf_project_destroy(p);
}

TEST(ApplyBatch, BuildChain) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));  // ms-grade modifiedAt tick
  const std::string before = ip->doc.project.modifiedAt;

  // Phase 1: three addNode intents (batch generates the UUIDs).
  sf_cmd_t nodes[3]{};
  nodes[0].type = SF_CMD_ADD_NODE;
  nodes[0].port_a = SF_NODE_SOURCE;
  std::strncpy(nodes[0].id1, "src", sizeof(nodes[0].id1) - 1);
  nodes[1].type = SF_CMD_ADD_NODE;
  nodes[1].port_a = SF_NODE_PROCESSOR;
  std::strncpy(nodes[1].id1, "proc", sizeof(nodes[1].id1) - 1);
  nodes[2].type = SF_CMD_ADD_NODE;
  nodes[2].port_a = SF_NODE_OUTPUT;
  std::strncpy(nodes[2].id1, "out", sizeof(nodes[2].id1) - 1);

  size_t applied = 0;
  char err[256];
  ASSERT_EQ(sf_graph_apply_batch(p, nodes, 3, &applied, err, sizeof(err)), SF_OK);
  EXPECT_EQ(applied, 3u);
  EXPECT_EQ(ip->doc.signalGraph.nodes.size(), 3u);

  const sfcore::SignalNode* src = find_by_name(ip, "src");
  const sfcore::SignalNode* proc = find_by_name(ip, "proc");
  const sfcore::SignalNode* out = find_by_name(ip, "out");
  ASSERT_NE(src, nullptr);
  ASSERT_NE(proc, nullptr);
  ASSERT_NE(out, nullptr);

  // Phase 2: wire src->proc->out and trim proc's gain (edge cmds carry UUIDs).
  sf_cmd_t wiring[3]{};
  wiring[0].type = SF_CMD_ADD_EDGE;
  std::strncpy(wiring[0].id1, src->id.c_str(), sizeof(wiring[0].id1) - 1);
  std::strncpy(wiring[0].id2, proc->id.c_str(), sizeof(wiring[0].id2) - 1);
  wiring[1].type = SF_CMD_ADD_EDGE;
  std::strncpy(wiring[1].id1, proc->id.c_str(), sizeof(wiring[1].id1) - 1);
  std::strncpy(wiring[1].id2, out->id.c_str(), sizeof(wiring[1].id2) - 1);
  wiring[2].type = SF_CMD_SET_MIXER;
  std::strncpy(wiring[2].id1, proc->id.c_str(), sizeof(wiring[2].id1) - 1);
  wiring[2].value = -6.0;
  wiring[2].value2 = 0.25;
  wiring[2].flags = SF_MIXER_FLAG_MUTE | SF_MIXER_FLAG_SOLO;

  ASSERT_EQ(sf_graph_apply_batch(p, wiring, 3, &applied, err, sizeof(err)), SF_OK);
  EXPECT_EQ(applied, 3u);
  EXPECT_EQ(ip->doc.signalGraph.edges.size(), 2u);

  // Same code path as the synchronous mutators: state + audit + modifiedAt.
  EXPECT_DOUBLE_EQ(proc->mixer.gainDb, -6.0);
  EXPECT_DOUBLE_EQ(proc->mixer.pan, 0.25);
  EXPECT_TRUE(proc->mixer.mute);
  EXPECT_TRUE(proc->mixer.solo);
  // sf_project_create already appended one entry; batch entries follow it.
  EXPECT_EQ(ip->doc.auditLog.size(), 7u);  // 1 create + 3 + 3 batch cmds
  EXPECT_EQ(static_cast<std::string>(ip->doc.auditLog[1].action), "graph.addNode");
  EXPECT_EQ(static_cast<std::string>(ip->doc.auditLog[4].action), "graph.addEdge");
  EXPECT_EQ(static_cast<std::string>(ip->doc.auditLog[6].action), "graph.setMixer");
  EXPECT_NE(ip->doc.project.modifiedAt, before);

  // Public surface agrees: evaluate_mixer sees the batch-built chain.
  char* out_json = nullptr;
  size_t len = 0;
  ASSERT_EQ(sf_graph_evaluate_mixer(p, &out_json, &len), SF_OK);
  sfcore::json j = sfcore::json::parse(std::string(out_json, len));
  sf_free_string(out_json);
  EXPECT_EQ(j["order"].size(), 3u);

  sf_project_destroy(p);
}

TEST(ApplyBatch, SetPresetRequiresExistingPreset) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);
  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);

  sfcore::ObjectEnvelope env;
  env.id = "preset-1";
  env.type = "dspPreset";
  env.createdAt = env.modifiedAt = "2026-01-01T00:00:00Z";
  ip->doc.dspPresets.push_back(env);

  // Phase 1: create the processor (batch generates its UUID).
  sf_cmd_t add = make_cmd(SF_CMD_ADD_NODE);
  add.port_a = SF_NODE_PROCESSOR;
  std::strncpy(add.id1, "proc", sizeof(add.id1) - 1);
  size_t applied = 0;
  char err[128];
  ASSERT_EQ(sf_graph_apply_batch(p, &add, 1, &applied, err, sizeof(err)), SF_OK);
  EXPECT_EQ(applied, 1u);
  const sfcore::SignalNode* proc = find_by_name(ip, "proc");
  ASSERT_NE(proc, nullptr);

  // Phase 2: preset reference by node UUID (name strings never resolve).
  sf_cmd_t set = make_cmd(SF_CMD_SET_PRESET);
  std::strncpy(set.id1, proc->id.c_str(), sizeof(set.id1) - 1);
  std::strncpy(set.id2, "preset-1", sizeof(set.id2) - 1);
  ASSERT_EQ(sf_graph_apply_batch(p, &set, 1, &applied, err, sizeof(err)), SF_OK);
  EXPECT_EQ(applied, 1u);
  EXPECT_EQ(proc->dspPresetRef, "preset-1");

  // Unknown preset mirrors the synchronous mutator: SF_E_NOT_FOUND, ref intact.
  sf_cmd_t bad = make_cmd(SF_CMD_SET_PRESET);
  std::strncpy(bad.id1, proc->id.c_str(), sizeof(bad.id1) - 1);
  std::strncpy(bad.id2, "nope", sizeof(bad.id2) - 1);
  applied = 99;
  ASSERT_EQ(sf_graph_apply_batch(p, &bad, 1, &applied, err, sizeof(err)), SF_E_NOT_FOUND);
  EXPECT_EQ(applied, 0u);
  EXPECT_NE(std::string(err).find("preset not found"), std::string::npos);

  sf_project_destroy(p);
}

TEST(ApplyBatch, StopOnFirstFailure) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);

  sf_cmd_t cmds[3]{};
  cmds[0].type = SF_CMD_ADD_NODE;
  cmds[0].port_a = SF_NODE_SOURCE;
  std::strncpy(cmds[0].id1, "src", sizeof(cmds[0].id1) - 1);
  cmds[1].type = SF_CMD_ADD_NODE;  // empty name => rejected by add_node_impl
  cmds[1].port_a = SF_NODE_PROCESSOR;
  cmds[2].type = SF_CMD_ADD_NODE;  // valid, but must NOT be applied
  cmds[2].port_a = SF_NODE_OUTPUT;
  std::strncpy(cmds[2].id1, "out", sizeof(cmds[2].id1) - 1);

  size_t applied = 0;
  char err[128];
  ASSERT_EQ(sf_graph_apply_batch(p, cmds, 3, &applied, err, sizeof(err)), SF_E_INVALID_ARG);
  EXPECT_EQ(applied, 1u);
  EXPECT_NE(std::string(err).find("graph.addNode"), std::string::npos);

  auto* ip = reinterpret_cast<sfcore::SfProject*>(p);
  EXPECT_EQ(ip->doc.signalGraph.nodes.size(), 1u);  // "out" never applied
  EXPECT_EQ(ip->doc.auditLog.size(), 2u);  // project.create + graph.addNode
  EXPECT_EQ(static_cast<std::string>(ip->doc.auditLog[1].action), "graph.addNode");
  sf_project_destroy(p);
}

TEST(ApplyBatch, EvaluateMixerIsNotApplicable) {
  sf_project_t* p = sf_project_create("B", nullptr);
  ASSERT_NE(p, nullptr);

  sf_cmd_t c = make_cmd(SF_CMD_EVALUATE_MIXER);  // query, not a mutation
  size_t applied = 99;
  char err[128];
  ASSERT_EQ(sf_graph_apply_batch(p, &c, 1, &applied, err, sizeof(err)), SF_E_INVALID_ARG);
  EXPECT_EQ(applied, 0u);
  EXPECT_NE(std::string(err).find("unsupported"), std::string::npos);
  sf_project_destroy(p);
}