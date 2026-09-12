// SoundForge G0 — crash-safe ring logging, error store, log sink, flush,
// capability probe stub, and diagnostics-related C ABI entry points (§6).
#include "sf_internal.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "soundforge/sf_diagnostics.h"
#include "soundforge/sf_project.h"

namespace sfcore {
namespace {

constexpr size_t kRingCap = 64 * 1024;

struct Ring {
  std::mutex mu;
  char buf[kRingCap];
  size_t head = 0;
  size_t count = 0;
  bool wrapped = false;

  void push(const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      buf[head] = data[i];
      head = (head + 1) % kRingCap;
      if (count < kRingCap) {
        ++count;
      } else {
        wrapped = true;  // drop oldest
      }
    }
  }

  std::string drain() {
    std::string out;
    out.reserve(count);
    const size_t start = wrapped ? head : 0;
    for (size_t i = 0; i < count; ++i) out.push_back(buf[(start + i) % kRingCap]);
    head = 0;
    count = 0;
    wrapped = false;
    return out;
  }
};

Ring& ring() {
  static Ring r;
  return r;
}

const char* level_name(int level) {
  static const char* kNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  return kNames[level];
}

std::atomic<sf_log_sink_fn> g_sink{nullptr};

std::mutex g_globalErrMu;
std::string g_globalErr = "no error";

thread_local std::string t_lastErr = "no error";

}  // namespace

void set_last_error(const std::string& msg) {
  t_lastErr = msg.empty() ? "unknown error" : msg;
  set_global_last_error(msg);  // keep sf_last_error_global() a live public channel
}

void set_global_last_error(const std::string& msg) {
  std::lock_guard<std::mutex> lk(g_globalErrMu);
  g_globalErr = msg.empty() ? "unknown error" : msg;
}

const char* thread_last_error_cstr() { return t_lastErr.c_str(); }

const char* global_last_error_cstr() {
  // G0 limitation: sets are rare; a concurrent set while a caller reads may
  // race. Accepted for G0 (documented in sf_diagnostics.h contract notes).
  std::lock_guard<std::mutex> lk(g_globalErrMu);
  return g_globalErr.c_str();
}

void log_line(int level, const char* tag, const char* msg) {
  if (!msg) msg = "";
  if (!tag) tag = "core";
  if (level < SF_LOG_DEBUG) level = SF_LOG_DEBUG;
  if (level > SF_LOG_ERROR) level = SF_LOG_ERROR;

  char ts[40];
  now_iso8601_raw(ts, sizeof(ts));
  char line[600];
  const int n = std::snprintf(line, sizeof(line), "[%s][%s][%s] %.480s\n", ts,
                              level_name(level), tag, msg);
  if (n <= 0) return;
  const size_t len = static_cast<size_t>(n) < sizeof(line) ? static_cast<size_t>(n) : sizeof(line);
  {
    auto& r = ring();
    std::lock_guard<std::mutex> lk(r.mu);
    r.push(line, len);
  }
  const sf_log_sink_fn sink = g_sink.load(std::memory_order_relaxed);
  if (sink) sink(static_cast<sf_log_level_t>(level), tag, msg);
}

}  // namespace sfcore

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------
extern "C" void sf_log(sf_log_level_t level, const char* tag, const char* msg) {
  sfcore::log_line(static_cast<int>(level), tag, msg);
}

extern "C" void sf_set_log_sink(sf_log_sink_fn sink) {
  sfcore::g_sink.store(sink, std::memory_order_relaxed);
}

extern "C" sf_result_t sf_flush_logs(const char* path) {
  if (!path || !*path) {
    sfcore::set_last_error("flush_logs: null path");
    return SF_E_INVALID_ARG;
  }
  try {
    const std::string contents = sfcore::ring().drain();
    if (contents.empty()) return SF_OK;
    std::FILE* f = std::fopen(path, "ab");
    if (!f) {
      sfcore::set_last_error(std::string("flush_logs: cannot open '") + path + "'");
      return SF_E_IO;
    }
    const size_t w = std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
    if (w != contents.size()) {
      sfcore::set_last_error("flush_logs: short write");
      return SF_E_IO;
    }
    return SF_OK;
  } catch (...) {
    sfcore::set_last_error("flush_logs: unknown error");
    return SF_E_IO;
  }
}

extern "C" sf_result_t sf_capability_probe_json(char* buf, size_t cap) {
  if (!buf || cap == 0) {
    sfcore::set_last_error("capability_probe: null buffer");
    return SF_E_INVALID_ARG;
  }
  try {
    sfcore::json j;
    j["osVersion"] = "unknown";        // G0 stub: no HAL touch (§6.1)
    j["abi"] = "unknown";
    j["lowLatencySupported"] = false;
    j["sampleRates"] = sfcore::json::array({44100, 48000});
    j["channelCounts"] = sfcore::json::array({2});
    j["timestamp"] = sfcore::now_iso8601();
    j["engineVersion"] = sf_engine_version();
    const std::string s = j.dump(2);
    if (s.size() + 1 > cap) {
      sfcore::set_last_error("capability_probe: buffer too small");
      return SF_E_NOMEM;
    }
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return SF_OK;
  } catch (...) {
    sfcore::set_last_error("capability_probe: serialization failed");
    return SF_E_NOMEM;
  }
}

extern "C" void sf_free_string(char* s) { std::free(s); }

extern "C" const char* sf_last_error(const sf_project_t* p) {
  if (p) return reinterpret_cast<const sfcore::SfProject*>(p)->lastError.c_str();
  return sfcore::thread_last_error_cstr();
}

extern "C" const char* sf_last_error_global(void) { return sfcore::global_last_error_cstr(); }
