// SoundForge G3 P4a — sf_queue_runner (PLAN_G3 §4.3 D3-amd).
//
// The runner is the live consumer of the SPSC command queue: started on its
// own std::thread, it polls the queue (yield + ~1 ms sleep when empty), drains
// up to kRunnerBatch commands, and applies them on its own thread via
// sfcore::apply_batch_impl — the exact internal loop the sync ABI entry uses,
// so a queued command mutates the project identically (audit + modifiedAt
// bumps included). Two command types are intercepted BEFORE apply_batch:
//
//   SF_CMD_STOP (0)          — control: stop filling the batch and exit the
//                              drain loop; commands behind STOP stay queued.
//   SF_CMD_EVALUATE_MIXER(7) — query: evaluate_mixer(g) is stored as the
//                              bounded single-slot last_report.
//
// Single-owner discipline (SEC-G3-1/-G3-3/-G3-4): while RUNNING/STOPPING the
// runner is the only thread that mutates the handle; the public
// sf_graph_apply_batch rejects external callers (C7). The runner NEVER writes
// proj->lastError. The RUNNING->STOPPED transition is performed ONLY by the
// runner thread's epilogue, so join-return implies the state is settled.

#include "sf_internal.hpp"
#include "graph_internal.hpp"

#include "soundforge/sf_command_queue.h"
#include "soundforge/sf_queue_runner.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>

using namespace sfcore;

namespace {

constexpr size_t kRunnerBatch = 64;        // commands applied per drain batch
constexpr size_t kRunnerReportCap = 8192;  // last_report JSON content bytes

// Human-readable handle id for the §5.4 log rows — the project pointer is the
// stable identity of the bound handle and avoids touching `doc` from stop().
std::string handle_id(const sfcore::SfProject* p) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%p", static_cast<const void*>(p));
  return buf;
}

// Bounded, always-valid serialization of an evaluate_mixer report (ORC-3).
// Fast path: a dump that fits the cap is stored verbatim. On overflow we scan
// the dumped object tracking string/depth state and keep the longest prefix
// ending at a top-level member boundary (a depth-1 comma outside a string)
// such that prefix + `,"_truncated": true}` still fits: that is always valid
// JSON because the prefix ends between complete members. Degenerate case (no
// top-level boundary fits): a minimal sentinel object carrying the omitted
// byte count. Never splits a string, never emits invalid JSON.
std::string bounded_report(const json& j) {
  const std::string full = j.dump(2);
  if (full.size() <= kRunnerReportCap) return full;
  static const char kSentinel[] = ",\"_truncated\": true}";
  const size_t suffix_len = sizeof(kSentinel) - 1;
  size_t best = std::string::npos;
  bool in_string = false;
  bool escape = false;
  int depth = 0;
  for (size_t i = 0; i < full.size(); ++i) {
    const char c = full[i];
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
    } else if (c == ',' && depth == 1) {
      // Prefix full[0,i) ends just before this separator: complete members.
      if (i + suffix_len <= kRunnerReportCap) {
        best = i;
      } else {
        break;  // later boundaries are only larger
      }
    }
  }
  if (best != std::string::npos) return full.substr(0, best) + kSentinel;
  return std::string("{\"_truncated\": true, \"_omitted_bytes\": ") +
         std::to_string(full.size()) + "}";
}

}  // namespace

// Opaque handle (matches the typedef tag in sf_queue_runner.h).
struct sf_queue_runner_s {
  sf_cmd_queue_t* queue = nullptr;
  sf_project_t* project = nullptr;         // borrowed; must outlive the runner
  SfProject* proj = nullptr;               // typed view of `project`
  std::atomic<int32_t>* state = nullptr;   // -> proj->runnerState
  std::thread thread;
  mutable std::mutex report_mu;
  std::string report;                      // latest EVALUATE_MIXER dump
  bool has_report = false;
};

namespace {

// Drain loop body — run on the runner thread. Any escaping exception would
// call std::terminate, so runner_main wraps this whole function.
void run_loop(sf_queue_runner_s* self) {
  sf_cmd_queue_t* const q = self->queue;
  auto* const state = self->state;
  bool stop_requested = false;

  while (!stop_requested) {
    if (state->load(std::memory_order_acquire) == SfRunnerStopping) break;

    sf_cmd_t batch[kRunnerBatch];
    size_t n = 0;        // mutation commands accumulated for apply_batch
    size_t fetched = 0;  // dequeues examined this pass (bounds control cmds)
    while (fetched < kRunnerBatch) {
      sf_cmd_t cmd{};
      if (sf_cmd_queue_dequeue(q, &cmd) != SF_OK) break;
      ++fetched;
      if (cmd.type == SF_CMD_STOP) {
        stop_requested = true;  // ORC-2 barrier: stop filling here
        break;
      }
      if (cmd.type == SF_CMD_EVALUATE_MIXER) {
        // Query, not a mutation: intercept before apply_batch (never applied).
        const json result = sfcore::evaluate_mixer(self->proj->doc.signalGraph);
        {
          std::lock_guard<std::mutex> lk(self->report_mu);
          self->report = bounded_report(result);
          self->has_report = true;
        }
        log_line(SF_LOG_INFO, "runner",
                 ("evaluate ok seq=" + std::to_string(cmd.seq)).c_str());
        continue;
      }
      batch[n++] = cmd;
    }

    if (n > 0) {
      // One apply per drained batch (the sync loop semantics: stop at the
      // first failing command). A partial failure is a §5.4 ERROR row; the
      // remaining buffered commands were already dequeued and are dropped.
      size_t applied = 0;
      std::string err;
      const sf_result_t rc = apply_batch_impl(self->project, batch, n, &applied, &err);
      if (rc != SF_OK) {
        log_line(SF_LOG_ERROR, "runner",
                 ("apply_batch stopped at " + std::to_string(applied) + "/" +
                  std::to_string(n) + ": " + err)
                     .c_str());
      }
    } else if (fetched == 0 && !stop_requested) {
      // Idle poll: no busy spin (plan §4.3).
      std::this_thread::yield();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void runner_main(sf_queue_runner_s* self) {
  auto* const state = self->state;  // copied: no *self deref after STOPPED store
  try {
    run_loop(self);
  } catch (const std::exception& e) {
    log_line(SF_LOG_ERROR, "runner",
             (std::string("runner crashed: ") + e.what()).c_str());
  } catch (...) {
    log_line(SF_LOG_ERROR, "runner", "runner crashed: unknown exception");
  }
  // SEC-G3-3: only the runner epilogue publishes RUNNING/STOPPING -> STOPPED.
  state->store(SfRunnerStopped, std::memory_order_release);
}

}  // namespace

extern "C" {

sf_result_t sf_queue_runner_create(sf_queue_runner_t** out, sf_cmd_queue_t* q,
                                   sf_project_t* p) {
  if (!out || !q || !p) {
    set_last_error("runner.create: null argument");
    return SF_E_INVALID_ARG;
  }
  *out = nullptr;
  auto* proj = reinterpret_cast<SfProject*>(p);
  if (proj->runnerState.load(std::memory_order_acquire) != SfRunnerIdle) {
    // One runner per project handle, one-shot (nothing resets STOPPED).
    set_handle_error(proj, "runner.create: project not idle");
    return SF_E_IO;
  }
  auto* r = new (std::nothrow) sf_queue_runner_s();
  if (!r) {
    set_last_error("runner.create: out of memory");
    return SF_E_NOMEM;
  }
  r->queue = q;
  r->project = p;
  r->proj = proj;
  r->state = &proj->runnerState;
  *out = r;
  return SF_OK;
}

sf_result_t sf_queue_runner_start(sf_queue_runner_t* r) {
  if (!r) {
    set_last_error("runner.start: null argument");
    return SF_E_INVALID_ARG;
  }
  int32_t expected = SfRunnerIdle;
  if (!r->state->compare_exchange_strong(expected, SfRunnerRunning,
                                         std::memory_order_acq_rel)) {
    set_handle_error(r->proj, "project.runner: already active");
    return SF_E_IO;
  }
  try {
    r->thread = std::thread(runner_main, r);
  } catch (const std::exception& e) {
    r->state->store(SfRunnerIdle, std::memory_order_release);
    set_handle_error(r->proj, "runner.start: thread spawn failed");
    return SF_E_IO;
  } catch (...) {
    r->state->store(SfRunnerIdle, std::memory_order_release);
    set_handle_error(r->proj, "runner.start: thread spawn failed");
    return SF_E_IO;
  }
  log_line(SF_LOG_INFO, "runner",
           ("runner start handle=" + handle_id(r->proj)).c_str());
  return SF_OK;
}

sf_result_t sf_queue_runner_stop(sf_queue_runner_t* r) {
  if (!r) {
    set_last_error("runner.stop: null argument");
    return SF_E_INVALID_ARG;
  }
  int32_t expected = SfRunnerRunning;
  if (r->state->compare_exchange_strong(expected, SfRunnerStopping,
                                        std::memory_order_acq_rel)) {
    log_line(SF_LOG_INFO, "runner",
             ("runner stop handle=" + handle_id(r->proj)).c_str());
    return SF_OK;
  }
  // Idempotent: IDLE (never started), already STOPPING or STOPPED -> no-op.
  return SF_OK;
}

sf_result_t sf_queue_runner_join(sf_queue_runner_t* r) {
  if (!r) {
    set_last_error("runner.join: null argument");
    return SF_E_INVALID_ARG;
  }
  if (r->thread.joinable()) r->thread.join();  // idempotent; no-op if never started
  return SF_OK;
}

sf_result_t sf_queue_runner_last_report(const sf_queue_runner_t* r, char* buf,
                                        size_t cap) {
  if (!r || !buf || cap == 0) {
    set_last_error("runner.lastReport: null argument");
    return SF_E_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lk(r->report_mu);
  if (!r->has_report) {
    set_last_error("runner.lastReport: no report yet");
    return SF_E_IO;
  }
  if (r->report.size() + 1 > cap) {
    set_last_error("runner.lastReport: buffer too small");
    return SF_E_NOMEM;
  }
  std::memcpy(buf, r->report.c_str(), r->report.size() + 1);
  return SF_OK;
}

sf_result_t sf_queue_runner_destroy(sf_queue_runner_t* r) {
  if (!r) return SF_OK;  // safe no-op
  const int32_t st = r->state->load(std::memory_order_acquire);
  if (runner_thread_alive(st)) {
    // Async destroy cannot be honored without racing the live thread: skip the
    // free and let the caller observe via sf_last_error(handle) (SEC-G3-2).
    log_line(SF_LOG_ERROR, "project", "destroy: queue runner active");
    set_handle_error(r->proj, "project.destroy: queue runner active");
    return SF_E_IO;
  }
  // IDLE/STOPPED only. A started-but-unjoined thread is reaped first so
  // std::thread's destructor can never call std::terminate.
  if (r->thread.joinable()) r->thread.join();
  delete r;
  return SF_OK;
}

}  // extern "C"
