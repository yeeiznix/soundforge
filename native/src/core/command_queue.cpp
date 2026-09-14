// SoundForge G2 P5 — lock-free SPSC command queue + apply_batch (§4.4, §4.5).
//
// The hot path (enqueue/dequeue) is malloc-free and lock-free BY CONSTRUCTION:
// a fixed array of fixed-size sf_cmd_t slots (176 B, capacity 256 = 44 KiB),
// one acquire load, one release store and one fixed-slot copy per op. No heap,
// no mutex, no CAS anywhere in push/pop — strictly wait-free (stronger than the
// plan's "reserve/commit single CAS"). The 10k threaded test exercises ~39 full
// drain cycles and runs clean under ASan/UBSan.
//
// sf_graph_apply_batch reuses the SAME impl helpers as the synchronous graph
// mutators, so a queued command mutates a project exactly like a direct call
// (audit entries + modifiedAt bumps included).

#include <atomic>
#include <cstring>
#include <new>
#include <string>

#include "graph_internal.hpp"
#include "sf_internal.hpp"

#include "soundforge/sf_command_queue.h"

using namespace sfcore;

namespace {

constexpr uint32_t kCmdCapacity = 256;  // power of two (plan §4.4)
constexpr uint32_t kCmdMask = kCmdCapacity - 1;

// Classic Lamport SPSC ring. Producer owns `tail` + `seq_next`; consumer owns
// `head`. Each side only ever LOADS the other's index, so there is no
// compare-and-swap anywhere on the hot path. Memory ordering:
//   enqueue: slot write -> tail.release ;  dequeue: tail.acquire -> slot read
//   dequeue: slot read  -> head.release ;  enqueue: head.acquire -> slot write
// guarantees the producer never overwrites a slot the consumer is still
// reading (full check protects reuse) and the consumer never reads a slot
// before its data is visible.
struct SfCmdQueue {
  std::atomic<uint32_t> head{0};  // next slot to consume (consumer-owned)
  std::atomic<uint32_t> tail{0};  // next slot to fill (producer-owned)
  uint64_t seq_next = 1;          // producer-only monotonic watermark (C4)
  sf_cmd_t slots[kCmdCapacity];   // fixed-size slots; never grows
};

}  // namespace

extern "C" {

sf_result_t sf_cmd_queue_create(sf_cmd_queue_t** out) {
  if (!out) {
    set_last_error("cmdqueue.create: null argument");
    return SF_E_INVALID_ARG;
  }
  *out = nullptr;
  auto* q = new (std::nothrow) SfCmdQueue();
  if (!q) {
    set_last_error("cmdqueue.create: out of memory");
    return SF_E_NOMEM;
  }
  *out = reinterpret_cast<sf_cmd_queue_t*>(q);
  return SF_OK;
}

void sf_cmd_queue_destroy(sf_cmd_queue_t* q) {
  delete reinterpret_cast<SfCmdQueue*>(q);  // delete NULL is a safe no-op
}

sf_result_t sf_cmd_queue_enqueue(sf_cmd_queue_t* q, const sf_cmd_t* cmd) {
  if (!q || !cmd) {
    set_last_error("cmdqueue.enqueue: null argument");
    return SF_E_INVALID_ARG;
  }
  auto* self = reinterpret_cast<SfCmdQueue*>(q);
  const uint32_t tail = self->tail.load(std::memory_order_relaxed);
  const uint32_t head = self->head.load(std::memory_order_acquire);
  if (tail - head >= kCmdCapacity) {
    // Overflow: reject + WARN (plan §4.4) — never block, never drop-oldest.
    // The watermark does NOT advance: a dropped message never consumed a seq
    // number, so accepted command seqs stay dense/gapless even if callers
    // retry the same message (G3 producer) after a transient full. The WARN
    // reports the seq the message would have received (reused on retry).
    const uint64_t dropped = self->seq_next;
    log_line(SF_LOG_WARN, "cmdqueue",
             ("cmdqueue: overflow dropped seq=" + std::to_string(dropped)).c_str());
    return SF_E_IO;
  }
  sf_cmd_t slot = *cmd;        // fixed-size copy — the only real work here
  slot.seq = self->seq_next;   // seq assigned by the queue, not the caller
  self->seq_next += 1;
  self->slots[tail & kCmdMask] = slot;
  self->tail.store(tail + 1, std::memory_order_release);
  return SF_OK;
}

sf_result_t sf_cmd_queue_dequeue(sf_cmd_queue_t* q, sf_cmd_t* out) {
  if (!q || !out) {
    set_last_error("cmdqueue.dequeue: null argument");
    return SF_E_INVALID_ARG;
  }
  auto* self = reinterpret_cast<SfCmdQueue*>(q);
  const uint32_t head = self->head.load(std::memory_order_relaxed);
  const uint32_t tail = self->tail.load(std::memory_order_acquire);
  if (head == tail) {
    set_last_error("cmdqueue.dequeue: queue empty");
    return SF_E_IO;  // not a kept error state; caller may retry (C2)
  }
  *out = self->slots[head & kCmdMask];
  self->head.store(head + 1, std::memory_order_release);
  return SF_OK;
}

int32_t sf_cmd_queue_depth(const sf_cmd_queue_t* q) {
  if (!q) return -1;
  const auto* self = reinterpret_cast<const SfCmdQueue*>(q);
  return static_cast<int32_t>(self->tail.load(std::memory_order_relaxed) -
                              self->head.load(std::memory_order_relaxed));
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Drain + apply (C3): applies queued intent via the SAME impl helpers as the
// synchronous mutators. Sequential; stops at the first failing command and
// reports its error.
//
// SEC-G3-1 split: `apply_batch_impl` is the loop with NO ownership check and
// NO handle-error write — the runner calls it directly on its own thread
// (SEC-G3-4: the runner never writes proj->lastError). The public
// `sf_graph_apply_batch` is the sanitized ABI entry: one acquire load of the
// runner state (C7 busy-reject while a runner is active), otherwise forward
// and mirror a failure into the handle error store + caller err_buf.
// ---------------------------------------------------------------------------
namespace sfcore {

sf_result_t apply_batch_impl(sf_project_t* p, const sf_cmd_t* cmds, size_t n,
                             size_t* applied, std::string* err_out) {
  if (applied) *applied = 0;
  if (!p || !applied || (n > 0 && !cmds)) return SF_E_INVALID_ARG;
  try {
    auto* proj = reinterpret_cast<SfProject*>(p);
    size_t done = 0;
    for (size_t i = 0; i < n; ++i) {
      const sf_cmd_t& cmd = cmds[i];
      std::string err;
      std::string action;
      std::string obj;
      std::string detail;
      sf_result_t rc = SF_OK;
      switch (cmd.type) {
        case SF_CMD_ADD_NODE: {
          std::string id = uuid_generate();
          rc = add_node_impl(proj->doc.signalGraph, cmd.port_a, cmd.id1, id, err);
          action = "graph.addNode";
          obj = id;
          detail = "kind=" + std::to_string(cmd.port_a) + " name=" + std::string(cmd.id1);
          break;
        }
        case SF_CMD_REMOVE_NODE: {
          int edges_removed = 0;
          rc = remove_node_impl(proj->doc.signalGraph, cmd.id1, err, edges_removed);
          action = "graph.removeNode";
          obj = cmd.id1;
          detail = "cascaded=" + std::to_string(edges_removed) + " edges";
          break;
        }
        case SF_CMD_ADD_EDGE: {
          std::string id = uuid_generate();
          rc = add_edge_impl(proj->doc.signalGraph, id, cmd.id1, cmd.id2,
                             cmd.port_a, cmd.port_b, err);
          action = "graph.addEdge";
          obj = id;
          detail = "from=" + std::string(cmd.id1) + " to=" + std::string(cmd.id2);
          break;
        }
        case SF_CMD_REMOVE_EDGE: {
          rc = remove_edge_impl(proj->doc.signalGraph, cmd.id1, err);
          action = "graph.removeEdge";
          obj = cmd.id1;
          break;
        }
        case SF_CMD_SET_MIXER: {
          const bool mute = (cmd.flags & SF_MIXER_FLAG_MUTE) != 0;
          const bool solo = (cmd.flags & SF_MIXER_FLAG_SOLO) != 0;
          rc = set_mixer_impl(proj->doc.signalGraph, cmd.id1, cmd.value, cmd.value2,
                              mute, solo, err);
          action = "graph.setMixer";
          obj = cmd.id1;
          detail = "gain=" + std::to_string(cmd.value) + " pan=" + std::to_string(cmd.value2) +
                   " mute=" + std::to_string(mute) + " solo=" + std::to_string(solo);
          break;
        }
        case SF_CMD_SET_PRESET: {
          std::string preset(cmd.id2);
          if (!preset.empty()) {
            bool found = false;
            for (const ObjectEnvelope& env : proj->doc.dspPresets) {
              if (env.id == preset) {
                found = true;
                break;
              }
            }
            if (!found) {
              rc = SF_E_NOT_FOUND;
              err = "graph.setPreset: preset not found";
              break;
            }
          }
          rc = set_preset_impl(proj->doc.signalGraph, cmd.id1, preset, err);
          action = "graph.setPreset";
          obj = cmd.id1;
          detail = "preset=" + preset;
          break;
        }
        default:
          rc = SF_E_INVALID_ARG;
          err = "graph.applyBatch: unsupported cmd type " + std::to_string(cmd.type);
          break;
      }
      if (rc != SF_OK) {
        *applied = done;  // report progress: how many applied BEFORE the stop
        if (err_out) *err_out = err;
        return rc;
      }
      user_audit(proj, action.c_str(), obj, detail);  // per-cmd audit + modifiedAt
      done += 1;
    }
    *applied = done;
    return SF_OK;
  } catch (const std::exception& e) {
    if (err_out) *err_out = e.what();
    return SF_E_SCHEMA;
  } catch (...) {
    if (err_out) *err_out = "unknown native exception";
    return SF_E_SCHEMA;
  }
}

}  // namespace sfcore

extern "C" {

sf_result_t sf_graph_apply_batch(sf_project_t* p, const sf_cmd_t* cmds, size_t n,
                                 size_t* applied, char* err_buf, size_t err_cap) {
  if (applied) *applied = 0;
  if (!p || !applied || (n > 0 && !cmds) || (err_cap > 0 && !err_buf)) {
    set_handle_error(nullptr, "graph.applyBatch: null argument");
    return SF_E_INVALID_ARG;
  }
  auto* proj = reinterpret_cast<SfProject*>(p);
  if (runner_thread_alive(proj->runnerState.load(std::memory_order_acquire))) {
    // C7 single-owner guard: reject, never block/wait (D3-amd SEC-G3-1).
    // Every non-OK return explains itself via err_buf when one is offered.
    log_line(SF_LOG_WARN, "graph", "busy: queue runner active");
    set_handle_error(proj, "project.busy: queue runner active");
    if (err_cap > 0) {
      std::strncpy(err_buf, "project.busy: queue runner active", err_cap - 1);
      err_buf[err_cap - 1] = '\0';
    }
    return SF_E_IO;
  }
  std::string err;
  const sf_result_t rc = apply_batch_impl(p, cmds, n, applied, &err);
  if (rc != SF_OK) {
    set_handle_error(proj, err);  // sf_last_error(handle) sees the failure
    if (err_cap > 0) {
      std::strncpy(err_buf, err.c_str(), err_cap - 1);
      err_buf[err_cap - 1] = '\0';
    }
  }
  return rc;
}

}  // extern "C"