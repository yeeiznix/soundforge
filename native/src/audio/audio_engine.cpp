// SoundForge G4 P4 — sf_audio_engine (PLAN_G4 §4.2 D2, §4.3 D3 + SEC-G4-01/02,
// §4.5 D5, §4.6 D6, §6 P4).
//
// The engine is the host-side "device callback": it owns an internal
// sf_queue_runner (bound to the caller's queue + project) and a
// PlanSnapshotStore that the runner publishes into after every applied
// mutation. Two drive modes share ONE render path (render_internal):
//
//   * deterministic synthetic clock — sf_audio_engine_tick(e, frames)
//   * optional pacer thread       — sf_audio_engine_start(e, SF_AUDIO_ENGINE_PACE)
//
// Single-owner posture mirrors sf_queue_runner: create/configure/set_output/
// start/stop/join/destroy are single-threaded by caller contract; tick and the
// pacer are mutually exclusive (pacer_active gate). meter_json/reset_meters are
// safe from any thread and are serialized against the render lane by ONE engine
// mutex (SEC-G4-02 choice (a)) so TruePeak stays single-thread-owned.

#include "soundforge/sf_audio_engine.h"

#include "sf_internal.hpp"
#include "snapshot.hpp"
#include "render_plan.hpp"
#include "true_peak.hpp"
#include "dsp_internal.hpp"

#include "soundforge/sf_queue_runner.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>

using namespace sfcore;

namespace {

// Local engine lifecycle state (PLAN_G4 §4.5 "Engine state machine"). Kept
// private to this TU — it is NOT part of sf_internal.hpp (the project only
// carries the audioEngine claim slot).
enum SfEngineState : int {
  kEngineCreated = 0,
  kEngineRunning = 1,
  kEngineStopping = 2,
  kEngineStopped = 3,
};

// Default engine configuration (header contract).
constexpr int32_t kDefaultSampleRate = 48000;
constexpr int32_t kDefaultChannels = 2;
constexpr int32_t kDefaultMaxBlockFrames =
    static_cast<int32_t>(dsp::kBlockMaxSamples);

// Human-readable handle id for the §5.4 log rows — the project pointer is the
// stable identity of the bound handle (same convention as sf_queue_runner).
std::string handle_id(const SfProject* p) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%p", static_cast<const void*>(p));
  return buf;
}

}  // namespace

// Opaque handle (matches the typedef tag in sf_audio_engine.h).
struct sf_audio_engine_s {
  sf_cmd_queue_t* queue = nullptr;
  sf_project_t* project = nullptr;  // borrowed; must outlive the engine
  SfProject* proj = nullptr;        // typed view of `project`
  sf_queue_runner_t* runner = nullptr;  // internal, owned

  std::atomic<int> state{kEngineCreated};

  // PACE-vs-tick exclusion. `pacer_active` is stored BEFORE the engine state
  // CAS CREATED->RUNNING in start(), so a tick that observes RUNNING necessarily
  // observes pacer_active (release/acquire). It stays true after stop() until
  // the pacer is reaped, so a late tick still sees PACE mode.
  std::atomic<bool> pacer_active{false};
  // Pacer shutdown flag: false while the pacer thread should keep ticking.
  std::atomic<bool> pacer_stop{true};

  std::atomic<bool> plan_valid{false};
  std::atomic<uint64_t> blocks_rendered{0};

  std::string out_node_id;  // CREATED-only writer; read by the observer at publish
  sf_audio_engine_config_t cfg{};

  // Heap-held so start()'s rollback can restore a clean pre-start store.
  std::unique_ptr<PlanSnapshotStore> store;

  // SEC-G4-02 (a): ONE mutex serializes TruePeak::process/latch/reset AND
  // meter_json/reset_meters. The render kernels themselves are lock-free.
  mutable std::mutex meter_mu;
  sfmeasure::TruePeak meter;

  std::thread pacer_thread;

  // Render-lane scratch (owned by tick or the pacer, never both).
  dsp::AudioBlock render_block;
  float in_l[dsp::kBlockMaxSamples] = {};
  float in_r[dsp::kBlockMaxSamples] = {};
  std::string render_err;  // reused to avoid per-tick allocation after warmup
};

namespace {

// ---------------------------------------------------------------------------
// Table-driven meter JSON (SEC-G4-01): bounded, locale-independent to_chars
// serialization into a fixed local buffer, then exactly one sized copy.
// ---------------------------------------------------------------------------
struct JsonBuf {
  char* p;
  char* end;
  bool ok = true;

  void put(const char* s, std::size_t n) {
    if (!ok) return;
    if (p + n > end) {
      ok = false;
      return;
    }
    std::memcpy(p, s, n);
    p += n;
  }
  void lit(const char* s) { put(s, std::strlen(s)); }
  void i64(long long v) {
    if (!ok) return;
    const auto r = std::to_chars(p, end, v);
    if (r.ec != std::errc()) {
      ok = false;
      return;
    }
    p = r.ptr;
  }
  void u64(unsigned long long v) {
    if (!ok) return;
    const auto r = std::to_chars(p, end, v);
    if (r.ec != std::errc()) {
      ok = false;
      return;
    }
    p = r.ptr;
  }
  void dbl(double v) {
    if (!ok) return;
    if (!std::isfinite(v)) v = 0.0;  // meter contract keeps JSON valid
    const auto r = std::to_chars(p, end, v);
    if (r.ec != std::errc()) {
      ok = false;
      return;
    }
    p = r.ptr;
  }
  void boolean(bool b) { lit(b ? "true" : "false"); }
};

// The one render path (PLAN_G4 §4.2 D2): read -> render -> write -> meter.
// Preconditions (frames in [1, max_block_frames]) are validated by the caller
// (tick) or guaranteed by the pacer. Never returns an error for a render
// failure: silence is a valid output (unknown target, null plan, cyclic plan).
sf_result_t render_internal(sf_audio_engine_s* e, std::size_t frames) {
  const int32_t channels = e->cfg.channels;

  // 1. Input: zero first, then let io.read fill it. A NULL callback or a
  //    non-SF_OK return means "render the block as silence" (zeros).
  std::memset(e->in_l, 0, frames * sizeof(float));
  std::memset(e->in_r, 0, frames * sizeof(float));
  if (e->cfg.io.read != nullptr) {
    float* rows[2] = {e->in_l, e->in_r};
    if (e->cfg.io.read(e->cfg.io.user, rows, channels,
                       static_cast<int32_t>(frames)) != SF_OK) {
      std::memset(e->in_l, 0, frames * sizeof(float));
      std::memset(e->in_r, 0, frames * sizeof(float));
    }
  }

  dsp::AudioBlock& block = e->render_block;
  for (std::size_t j = 0; j < frames; ++j) {
    block.L[j] = e->in_l[j];
    block.R[j] = e->in_r[j];
  }
  block.n = frames;

  // 2. Render through the newest published plan. Null plan (first tick before
  //    any publish) or a failed render (unknown target) -> silence.
  dsp::RenderPlan* plan = e->store->acquire();
  bool rendered = false;
  if (plan != nullptr) {
    e->render_err.clear();
    rendered = dsp::render_chain_planned(*plan, block, e->render_err);
  }
  if (!rendered) {
    for (std::size_t j = 0; j < frames; ++j) {
      block.L[j] = 0.0f;
      block.R[j] = 0.0f;
    }
  }

  // 3. Output: the write result is intentionally NOT propagated (the block is
  //    already rendered); it never aborts the tick.
  if (e->cfg.io.write != nullptr) {
    const float* rows[2] = {block.L, block.R};
    (void)e->cfg.io.write(e->cfg.io.user, rows, channels,
                          static_cast<int32_t>(frames));
  }

  // 4. Meter: measured on the rendered output block, after render.
  {
    std::lock_guard<std::mutex> lk(e->meter_mu);
    float* rows[2] = {block.L, block.R};
    e->meter.process(rows, static_cast<std::size_t>(channels), frames);
  }

  e->blocks_rendered.fetch_add(1, std::memory_order_relaxed);
  return SF_OK;
}

// Pacer thread body: repeatedly render max_block_frames at a period derived
// from sample_rate, sub-sleeping in <=2 ms chunks so stop() is responsive.
void pacer_main(sf_audio_engine_s* e) {
  const std::size_t frames = static_cast<std::size_t>(e->cfg.max_block_frames);
  const int32_t sr = e->cfg.sample_rate > 0 ? e->cfg.sample_rate
                                            : kDefaultSampleRate;
  const auto period = std::chrono::nanoseconds(
      static_cast<int64_t>(1e9 * static_cast<double>(frames) /
                           static_cast<double>(sr)));
  const auto chunk = std::chrono::milliseconds(2);

  while (!e->pacer_stop.load(std::memory_order_acquire)) {
    render_internal(e, frames);

    auto remaining = period;
    while (remaining > std::chrono::nanoseconds::zero() &&
           !e->pacer_stop.load(std::memory_order_acquire)) {
      const auto step = remaining < chunk ? remaining : chunk;
      std::this_thread::sleep_for(step);
      remaining -= step;
    }
  }
}

// Runner publish observer (PLAN_G4 §4.1 D1): fired on the runner thread after
// a batch that applied >= 1 mutation. Reads the engine's snapshot store + out
// node id (both stable from start until observer detach). Never throws: the
// store's publish() contains compile exceptions.
void snapshot_publish(void* user, const SignalGraphDoc& graph) {
  auto* e = static_cast<sf_audio_engine_s*>(user);
  std::string err;
  const bool ok = e->store->publish(graph, e->out_node_id, err);
  if (ok) {
    e->plan_valid.store(true, std::memory_order_release);
    log_line(SF_LOG_DEBUG, "engine",
             ("plan published nodes=" + std::to_string(graph.nodes.size())).c_str());
  } else {
    e->plan_valid.store(false, std::memory_order_release);
    log_line(SF_LOG_ERROR, "engine",
             ("plan compile failed: " + err).c_str());
  }
}

}  // namespace

extern "C" {

sf_result_t sf_audio_engine_create(sf_audio_engine_t** out, sf_cmd_queue_t* q,
                                   sf_project_t* p) {
  if (!out || !q || !p) {
    set_last_error("audio engine: create: null argument");
    return SF_E_INVALID_ARG;
  }
  *out = nullptr;
  auto* proj = reinterpret_cast<SfProject*>(p);
  if (proj->runnerState.load(std::memory_order_acquire) != SfRunnerIdle) {
    set_handle_error(proj, "audio engine: create: project not idle");
    return SF_E_IO;
  }

  std::unique_ptr<sf_audio_engine_s> e(new (std::nothrow) sf_audio_engine_s());
  if (!e) {
    set_last_error("audio engine: create: out of memory");
    return SF_E_NOMEM;
  }
  e->store = std::unique_ptr<PlanSnapshotStore>(new (std::nothrow) PlanSnapshotStore());
  if (!e->store) {
    set_last_error("audio engine: create: out of memory");
    return SF_E_NOMEM;
  }
  e->queue = q;
  e->project = p;
  e->proj = proj;
  e->cfg.sample_rate = kDefaultSampleRate;
  e->cfg.channels = kDefaultChannels;
  e->cfg.max_block_frames = kDefaultMaxBlockFrames;
  e->cfg.io.user = nullptr;
  e->cfg.io.read = nullptr;
  e->cfg.io.write = nullptr;

  // The REAL single-engine guard (review R-D): atomically claim the internal
  // slot. sf_queue_runner_create only *checks* runnerState, it does not reserve
  // it, so this CAS is what makes two engines mutually exclusive.
  void* expected = nullptr;
  if (!proj->audioEngine.compare_exchange_strong(expected, static_cast<void*>(e.get()),
                                                 std::memory_order_acq_rel)) {
    set_handle_error(proj, "audio engine: create: engine already attached");
    return SF_E_IO;
  }

  // Claim held: create the internal runner. On failure roll the claim back so
  // no half-bound state survives.
  if (sf_queue_runner_create(&e->runner, q, p) != SF_OK) {
    proj->audioEngine.store(nullptr, std::memory_order_release);
    set_handle_error(proj, "audio engine: create: runner create failed");
    return SF_E_IO;
  }

  *out = e.release();
  return SF_OK;
}

sf_result_t sf_audio_engine_destroy(sf_audio_engine_t* e) {
  if (!e) return SF_OK;  // safe no-op

  const int st = e->state.load(std::memory_order_acquire);
  if (st == kEngineRunning || st == kEngineStopping) {
    // The pacer/runner threads are still live: freeing now would race them.
    log_line(SF_LOG_ERROR, "engine", "destroy: engine active");
    set_handle_error(e->proj, "audio engine: destroy: engine active");
    return SF_E_IO;
  }

  // R-D exact ordering: join pacer -> runner.stop -> runner.join -> detach
  // observer -> destroy runner -> free store -> release claim -> free engine.
  e->pacer_stop.store(true, std::memory_order_release);
  if (e->pacer_thread.joinable()) e->pacer_thread.join();
  sf_queue_runner_stop(e->runner);
  sf_queue_runner_join(e->runner);
  sf_queue_runner_set_observer(e->runner, nullptr);
  sf_queue_runner_destroy(e->runner);
  e->store->release();
  e->store.reset();

  // Release the single-engine claim BEFORE freeing (PLAN_G4 §4.5: the claim
  // release runs before/independently of freeing the engine).
  SfProject* const proj = e->proj;
  void* expected = static_cast<void*>(e);
  proj->audioEngine.compare_exchange_strong(expected, nullptr,
                                            std::memory_order_acq_rel);
  delete e;
  return SF_OK;
}

sf_result_t sf_audio_engine_configure(sf_audio_engine_t* e,
                                      const sf_audio_engine_config_t* cfg) {
  if (!e || !cfg) {
    set_last_error("audio engine: configure: null argument");
    return SF_E_INVALID_ARG;
  }
  if (e->state.load(std::memory_order_acquire) != kEngineCreated) {
    set_handle_error(e->proj, "audio engine: configure: engine not configurable");
    return SF_E_IO;
  }
  if (cfg->sample_rate <= 0) {
    set_handle_error(e->proj, "audio engine: configure: invalid sample rate");
    return SF_E_INVALID_ARG;
  }
  if (cfg->channels != kDefaultChannels) {
    set_handle_error(e->proj, "audio engine: configure: channels must be 2");
    return SF_E_INVALID_ARG;
  }
  if (cfg->max_block_frames < 1 ||
      static_cast<std::size_t>(cfg->max_block_frames) > dsp::kBlockMaxSamples) {
    set_handle_error(e->proj, "audio engine: configure: invalid max block frames");
    return SF_E_INVALID_ARG;
  }
  e->cfg = *cfg;
  return SF_OK;
}

sf_result_t sf_audio_engine_set_output(sf_audio_engine_t* e,
                                       const char* out_node_id) {
  if (!e) {
    set_last_error("audio engine: set_output: null argument");
    return SF_E_INVALID_ARG;
  }
  if (e->state.load(std::memory_order_acquire) != kEngineCreated) {
    set_handle_error(e->proj, "audio engine: set_output: engine not configurable");
    return SF_E_IO;
  }
  // NULL/"" selects "no output" (renders silence) as a VALID configuration
  // (ORC-G4-06).
  e->out_node_id = out_node_id ? out_node_id : "";
  return SF_OK;
}

sf_result_t sf_audio_engine_start(sf_audio_engine_t* e, uint32_t flags) {
  if (!e) {
    set_last_error("audio engine: start: null argument");
    return SF_E_INVALID_ARG;
  }
  if (e->state.load(std::memory_order_acquire) != kEngineCreated) {
    set_handle_error(e->proj, "audio engine: already started");
    return SF_E_IO;
  }

  // SEC-G4-05 ordered gate: check for a standalone runner BEFORE reading the
  // document or compiling snapshot #0. sf_queue_runner_create only *checks*
  // runnerState without reserving it, so a runner could have been created in
  // the create->start window.
  const int32_t rs = e->proj->runnerState.load(std::memory_order_acquire);
  if (runner_thread_alive(rs) || rs != static_cast<int32_t>(SfRunnerIdle)) {
    set_handle_error(e->proj, "audio engine: start: project not idle");
    return SF_E_IO;
  }

  // Snapshot #0 from the caller-owned IDLE document. A compile failure is
  // non-fatal: the engine still starts and renders silence until a valid plan
  // is published (planValid:false).
  {
    std::string err;
    const bool ok = e->store->publish(e->proj->doc.signalGraph, e->out_node_id, err);
    if (ok) {
      e->plan_valid.store(true, std::memory_order_release);
      log_line(SF_LOG_DEBUG, "engine",
               ("plan published nodes=" +
                std::to_string(e->proj->doc.signalGraph.nodes.size()))
                   .c_str());
    } else {
      e->plan_valid.store(false, std::memory_order_release);
      log_line(SF_LOG_ERROR, "engine", ("plan compile failed: " + err).c_str());
    }
  }

  // PACE flag is published BEFORE the RUNNING CAS so a tick that sees RUNNING
  // also sees pacer_active (SEC-G4-02 pace-vs-tick guard).
  const bool want_pace = (flags & SF_AUDIO_ENGINE_PACE) != 0;
  if (want_pace) e->pacer_active.store(true, std::memory_order_release);

  int expected = kEngineCreated;
  if (!e->state.compare_exchange_strong(expected, kEngineRunning,
                                        std::memory_order_acq_rel)) {
    e->pacer_active.store(false, std::memory_order_release);
    set_handle_error(e->proj, "audio engine: already started");
    return SF_E_IO;
  }

  // Attach the publish observer BEFORE the runner starts (R-D lifetime rule).
  RunnerObserver obs{static_cast<void*>(e), &snapshot_publish};
  sf_queue_runner_set_observer(e->runner, &obs);

  if (sf_queue_runner_start(e->runner) != SF_OK) {
    // Rollback: nothing of the session survives; the engine stays CREATED so
    // destroy still works.
    sf_queue_runner_set_observer(e->runner, nullptr);
    // ORC-G4P4-01 (SEC-G4-04): never install a null store — the engine must
    // stay destroy()-safe and start()-retry-safe even under OOM at rollback.
    // Mirror create()'s guarded pattern (~290): allocate into a local first and
    // only swap when non-null; on allocation failure the just-released (now
    // empty) store is KEPT — publish over stale READY slots is safe (claim_slot
    // overwrites, and a reader acquires only after release held_==-1).
    std::unique_ptr<PlanSnapshotStore> fresh_store(
        new (std::nothrow) PlanSnapshotStore());
    e->store->release();
    if (fresh_store) e->store = std::move(fresh_store);
    e->plan_valid.store(false, std::memory_order_release);
    e->pacer_active.store(false, std::memory_order_release);
    e->state.store(kEngineCreated, std::memory_order_release);
    set_handle_error(e->proj, "audio engine: start: runner start failed");
    return SF_E_IO;
  }

  if (want_pace) {
    e->pacer_stop.store(false, std::memory_order_release);
    try {
      e->pacer_thread = std::thread(pacer_main, e);
    } catch (const std::exception&) {
      log_line(SF_LOG_ERROR, "engine", "pacer: thread spawn failed");
      e->pacer_stop.store(true, std::memory_order_release);
      e->pacer_active.store(false, std::memory_order_release);
      sf_queue_runner_stop(e->runner);
      sf_queue_runner_join(e->runner);
      sf_queue_runner_set_observer(e->runner, nullptr);
      // ORC-G4P4-01 (SEC-G4-04): never install a null store — see the runner-
      // start rollback above; under OOM the just-released (now empty) store is
      // kept so the engine never carries a null store.
      std::unique_ptr<PlanSnapshotStore> fresh_store(
          new (std::nothrow) PlanSnapshotStore());
      e->store->release();
      if (fresh_store) e->store = std::move(fresh_store);
      e->plan_valid.store(false, std::memory_order_release);
      e->state.store(kEngineCreated, std::memory_order_release);
      set_handle_error(e->proj, "audio engine: start: pacer spawn failed");
      return SF_E_IO;
    } catch (...) {
      log_line(SF_LOG_ERROR, "engine", "pacer: thread spawn failed");
      e->pacer_stop.store(true, std::memory_order_release);
      e->pacer_active.store(false, std::memory_order_release);
      sf_queue_runner_stop(e->runner);
      sf_queue_runner_join(e->runner);
      sf_queue_runner_set_observer(e->runner, nullptr);
      // ORC-G4P4-01 (SEC-G4-04): never install a null store — see the runner-
      // start rollback above; under OOM the just-released (now empty) store is
      // kept so the engine never carries a null store.
      std::unique_ptr<PlanSnapshotStore> fresh_store(
          new (std::nothrow) PlanSnapshotStore());
      e->store->release();
      if (fresh_store) e->store = std::move(fresh_store);
      e->plan_valid.store(false, std::memory_order_release);
      e->state.store(kEngineCreated, std::memory_order_release);
      set_handle_error(e->proj, "audio engine: start: pacer spawn failed");
      return SF_E_IO;
    }
  }

  log_line(SF_LOG_INFO, "engine",
           ("engine start handle=" + handle_id(e->proj)).c_str());
  return SF_OK;
}

sf_result_t sf_audio_engine_stop(sf_audio_engine_t* e) {
  if (!e) {
    set_last_error("audio engine: stop: null argument");
    return SF_E_INVALID_ARG;
  }
  int expected = kEngineRunning;
  if (e->state.compare_exchange_strong(expected, kEngineStopping,
                                       std::memory_order_acq_rel)) {
    e->pacer_stop.store(true, std::memory_order_release);
    sf_queue_runner_stop(e->runner);
    log_line(SF_LOG_INFO, "engine",
             ("engine stop handle=" + handle_id(e->proj)).c_str());
  }
  // Idempotent: CREATED (never started), STOPPING or STOPPED -> safe no-op.
  return SF_OK;
}

sf_result_t sf_audio_engine_join(sf_audio_engine_t* e) {
  if (!e) {
    set_last_error("audio engine: join: null argument");
    return SF_E_INVALID_ARG;
  }
  // join implies stop (bounded reaping).
  (void)sf_audio_engine_stop(e);
  if (e->pacer_thread.joinable()) e->pacer_thread.join();
  sf_queue_runner_join(e->runner);

  const int st = e->state.load(std::memory_order_acquire);
  if (st == kEngineRunning || st == kEngineStopping) {
    e->state.store(kEngineStopped, std::memory_order_release);
  }
  // CREATED (never started) stays CREATED; already STOPPED stays STOPPED.
  return SF_OK;
}

sf_result_t sf_audio_engine_tick(sf_audio_engine_t* e, size_t frames) {
  if (!e) {
    set_last_error("audio engine: tick: null argument");
    return SF_E_INVALID_ARG;
  }
  if (frames == 0 || frames > static_cast<std::size_t>(e->cfg.max_block_frames) ||
      frames > dsp::kBlockMaxSamples) {
    set_last_error("audio engine: tick: invalid frame count");
    return SF_E_INVALID_ARG;
  }
  if (e->state.load(std::memory_order_acquire) != kEngineRunning) {
    log_line(SF_LOG_WARN, "engine", "tick: engine not running");
    set_handle_error(e->proj, "audio engine: tick: engine not running");
    return SF_E_IO;
  }
  if (e->pacer_active.load(std::memory_order_acquire)) {
    // Pace vs tick concurrency guard (PLAN_G4 §4.2 review pointer (a)).
    set_handle_error(e->proj, "audio engine: tick: pacer active");
    return SF_E_IO;
  }
  return render_internal(e, frames);
}

sf_result_t sf_audio_engine_last_report(const sf_audio_engine_t* e, char* buf,
                                        size_t cap) {
  if (!e) {
    set_last_error("runner.lastReport: null argument");
    return SF_E_INVALID_ARG;
  }
  // Thin passthrough (PLAN_G4 §4.6 D6): identical bytes + error contract as the
  // internal runner. meter_json is NEVER embedded in this report.
  return sf_queue_runner_last_report(e->runner, buf, cap);
}

sf_result_t sf_audio_engine_meter_json(const sf_audio_engine_t* e, char* buf,
                                       size_t cap) {
  if (!e || !buf || cap == 0) {
    set_last_error("engine.meterJson: null argument");
    return SF_E_INVALID_ARG;
  }

  const int32_t channels = e->cfg.channels;
  double lin[2] = {0.0, 0.0};
  bool clip[2] = {false, false};
  {
    // One lock acquisition for BOTH channels: reset_meters cannot interleave.
    std::lock_guard<std::mutex> lk(e->meter_mu);
    for (int c = 0; c < 2; ++c) {
      const std::size_t uc = static_cast<std::size_t>(c);
      lin[c] = e->meter.latch(uc);
      clip[c] = e->meter.clipped(uc);
    }
  }
  const uint64_t blocks = e->blocks_rendered.load(std::memory_order_relaxed);
  const bool plan_valid = e->plan_valid.load(std::memory_order_acquire);

  char tmp[512];
  JsonBuf jb{tmp, tmp + sizeof(tmp) - 1};
  jb.lit("{\"channels\":");
  jb.i64(channels);
  jb.lit(",\"oversample\":");
  jb.u64(static_cast<unsigned long long>(sfmeasure::TruePeakOversample));
  jb.lit(",\"blocksRendered\":");
  jb.u64(static_cast<unsigned long long>(blocks));
  jb.lit(",\"truePeakLinear\":[");
  jb.dbl(lin[0]);
  jb.lit(",");
  jb.dbl(lin[1]);
  jb.lit("],\"truePeakDb\":[");
  for (int c = 0; c < 2; ++c) {
    if (c > 0) jb.lit(",");
    if (lin[c] > 0.0) {
      jb.dbl(20.0 * std::log10(lin[c]));
    } else {
      jb.lit("null");
    }
  }
  jb.lit("],\"clipped\":[");
  jb.boolean(clip[0]);
  jb.lit(",");
  jb.boolean(clip[1]);
  jb.lit("],\"planValid\":");
  jb.boolean(plan_valid);
  jb.lit("}");

  if (!jb.ok) {
    // Unreachable with the fixed 512-byte buffer / fixed key set.
    buf[0] = '\0';
    set_last_error("engine.meterJson: serialization overflow");
    return SF_E_IO;
  }
  const std::size_t len = static_cast<std::size_t>(jb.p - tmp);
  if (len + 1 > cap) {
    buf[0] = '\0';  // never a partial payload (SEC-G4-01)
    set_last_error("engine.meterJson: buffer too small");
    return SF_E_NOMEM;
  }
  std::memcpy(buf, tmp, len);
  buf[len] = '\0';
  return SF_OK;
}

sf_result_t sf_audio_engine_reset_meters(sf_audio_engine_t* e) {
  if (!e) {
    set_last_error("engine.resetMeters: null argument");
    return SF_E_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lk(e->meter_mu);
  e->meter.resetLatch();  // latches only; tails and blocksRendered preserved
  return SF_OK;
}

}  // extern "C"
