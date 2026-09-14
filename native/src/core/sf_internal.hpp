// Internal C++ core shared by native/src/core/*.cpp — NOT part of the public ABI.
// Canonical project data model (§9), codec, validation, migration, error store.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "soundforge/sf_command_queue.h"
#include "soundforge/sf_queue_runner.h"
#include "soundforge/sf_types.h"
#include "soundforge/sf_version.h"

// Typed graph model (§3.2) — must precede sfcore aliases below so the header
// stays self-contained (it uses concrete std/types only).
#include "graph_internal.hpp"

namespace sfcore {

using json = nlohmann::json;
using Uuid = std::string;          // canonical lower-case hyphenated v4
using IsoTimestamp = std::string;  // RFC3339 UTC, e.g. 2026-01-01T09:00:00.000Z

// ---------------------------------------------------------------------------
// Canonical key set (§9 closed document)
// ---------------------------------------------------------------------------
struct CollectionInfo {
  const char* key;
  const char* singular;
};

inline const std::vector<CollectionInfo>& collections() {
  static const std::vector<CollectionInfo> k = {
      {"audienceReceivers", "audienceReceiver"}, {"equipment", "equipment"},
      {"audioAssets", "audioAsset"},             {"dspPresets", "dspPreset"},
      {"arrayConfigurations", "arrayConfiguration"}, {"measurements", "measurement"},
      {"simulationRuns", "simulationRun"},       {"trainingScenarios", "trainingScenario"},
      {"inventoryRefs", "inventoryRef"},         {"reports", "report"},
  };
  return k;
}

inline const std::vector<std::string>& top_level_keys() {
  static const std::vector<std::string> k = {
      "schemaVersion",  "engineVersion", "project",   "venue",     "scene",
      "audienceReceivers", "equipment",   "signalGraph", "powerGraph", "audioAssets",
      "dspPresets",     "arrayConfigurations", "measurements", "simulationRuns",
      "trainingScenarios", "inventoryRefs", "reports", "auditLog",
  };
  return k;
}

// 8 MiB hard cap on project document size, enforced at read/input time
// (read_file_capped, sf_project_from_json) — never post-parse.
const size_t kMaxDocBytes = 8 * 1024 * 1024;

// FIFO cap on in-memory audit entries; oldest dropped first.
const size_t kMaxAuditEntries = 1000;

// Must stay in lockstep with project_schema.json auditLog[].action enum
// (13 items) — schema.json is the source of truth.
inline std::array<const char*, 13> audit_actions() {
  return {"project.create", "project.open", "project.save", "project.migrate",
          "project.rename", "venue.update", "scene.update", "graph.addNode",
          "graph.removeNode", "graph.addEdge", "graph.removeEdge", "graph.setMixer",
          "graph.setPreset"};
}

// ---------------------------------------------------------------------------
// Document model (§3.2)
// ---------------------------------------------------------------------------
struct AuditEntry {
  IsoTimestamp ts;
  std::string actor;    // "system" in G0
  std::string action;   // project.create|open|save|migrate
  Uuid objectId;
  std::string detail;   // truncated to 512 chars by callers
};

struct ProjectMeta {
  Uuid id;
  std::string name;
  IsoTimestamp createdAt;
  IsoTimestamp modifiedAt;
  std::string author;
  std::string notes;
};

struct VenueStub {
  Uuid id;
  std::string name;
  double widthM = 0.0;   // v2: room dimensions (meters), >0 when healthy
  double depthM = 0.0;
  double heightM = 0.0;
};

struct Point3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct SceneStub {
  Uuid id;
  std::string name;
  Uuid venueRef;
  Point3 center;     // v2: room-local meters, origin at front-left floor corner
  Point3 listening;  // v2: preferred listening position, same frame
};

// G1 room defaults — schema v2 venue.dimensions / scene.geometry (PLAN_G1 P2).
// Migration (migration.cpp), creation (project.cpp) and the Python mirror
// (migrate.py) MUST stay in lockstep with these values.
const double SF_ROOM_DEFAULT_W = 12.0;
const double SF_ROOM_DEFAULT_D = 10.0;
const double SF_ROOM_DEFAULT_H = 4.0;

struct ObjectEnvelope {
  Uuid id;
  std::string type;
  int version = 1;
  IsoTimestamp createdAt;
  IsoTimestamp modifiedAt;
  std::string provenance = "created";  // "created" | "migrated:v0->v1"
  json data = json::object();          // type-specific payload; empty in G0
};

inline json empty_graph() {
  json g = json::object();
  g["nodes"] = json::array();
  g["edges"] = json::array();
  return g;
}

struct SfProjectDoc {
  int schemaVersion = SF_SCHEMA_VERSION;
  std::string engineVersion;
  ProjectMeta project;
  VenueStub venue;
  SceneStub scene;
  std::vector<ObjectEnvelope> audienceReceivers;
  std::vector<ObjectEnvelope> equipment;
  SignalGraphDoc signalGraph;
  json powerGraph = empty_graph();
  std::vector<ObjectEnvelope> audioAssets;
  std::vector<ObjectEnvelope> dspPresets;
  std::vector<ObjectEnvelope> arrayConfigurations;
  std::vector<ObjectEnvelope> measurements;
  std::vector<ObjectEnvelope> simulationRuns;
  std::vector<ObjectEnvelope> trainingScenarios;
  std::vector<ObjectEnvelope> inventoryRefs;
  std::vector<ObjectEnvelope> reports;
  std::vector<AuditEntry> auditLog;
};

// Opaque handle owned by the native heap; Kotlin holds it as jlong.
//
// G3 P4a (PLAN_G3 §4.3 D3-amd): the handle carries the queue-runner lifecycle
// state as an atomic so any sync ABI entry can take ONE fast-path acquire load
// to enforce the single-owner guard (contract C7/C8) and so the runner
// thread's epilogue is the ONLY writer of RUNNING -> STOPPED (SEC-G3-3).
enum SfRunnerState : int {
  SfRunnerIdle = 0,     // no runner attached / never started or session undone
  SfRunnerRunning = 1,  // start() CASed IDLE->RUNNING; runner thread is live
  SfRunnerStopping = 2, // stop() CASed RUNNING->STOPPING; thread drains+exits
  SfRunnerStopped = 3   // runner thread epilogue published; join may reap
};

// True while the runner thread is or may still be alive (guard gate: sync ABI
// entries reject external access during the session; after STOPPED(joined)
// the ownership is returned to the caller — C8).
inline bool runner_thread_alive(int32_t st) {
  return st == SfRunnerRunning || st == SfRunnerStopping;
}

struct SfProject {
  SfProjectDoc doc;
  std::string lastError = "no error";
  // Queue-runner lifecycle (G3 P4a). Atomic so the ownership guard is a single
  // load (no flag/state desync, no TOCTOU beyond the documented forward race —
  // D3-amd SEC-G3-1/-G3-3). Only the runner thread's epilogue ever writes
  // STOPPED (after its last batch, before thread exit).
  std::atomic<int32_t> runnerState{static_cast<int32_t>(SfRunnerIdle)};
  // G4 P4 (internal claim slot; no public ABI, no new export/SF_E_*): the
  // single audio engine bound to this handle, or nullptr. sf_audio_engine_create
  // claims it with CAS(nullptr -> e) and rolls back on failure — this is the
  // REAL single-engine guard (review R-D): sf_queue_runner_create only CHECKS
  // runnerState == IDLE and does not reserve it, so two engines could otherwise
  // both pass create. void* keeps this header independent of the (P4) audio
  // module. The clone ctor does not copy it (see below).
  std::atomic<void*> audioEngine{nullptr};

  SfProject() = default;
  // ORC-1 (D3-amd): explicit copy ctor — sf_project_clone copies doc +
  // lastError but the runner lifecycle state defaults to IDLE: a clone never
  // inherits a runner (clone rejects while RUNNING via the P4b read guard, so
  // copying an active project is already fenced off by the ABI).
  SfProject(const SfProject& o) : doc(o.doc), lastError(o.lastError) {}
  // Assignment would copy an ACTIVE runner's state field onto another handle —
  // forbid the shape at compile time rather than define surprising semantics.
  SfProject& operator=(const SfProject&) = delete;
  SfProject& operator=(SfProject&&) = delete;
};

// Shared audit helper — used by project.cpp mutators and graph_abi.cpp.
// Detail is truncated to 512 bytes; auditLog is FIFO-capped at kMaxAuditEntries.
// Truncation walks back over UTF-8 continuation bytes (up to 3) so it never
// splits a multi-byte sequence (G3 ORC-P1-1): a split tail would make
// json.dump() throw under the strict handler. Even so, doc serialization uses
// error_handler_t::replace, so any malformed byte still serializes (as U+FFFD)
// instead of bricking the document.
IsoTimestamp now_iso8601();  // forward declaration (defined in uuid.cpp)

inline void user_audit(SfProject* proj, const char* action,
                       const Uuid& object_id, const std::string& detail) {
  const std::string now = now_iso8601();
  std::string d = detail;
  if (d.size() > 512) {
    d.resize(512);
    // Do not end on a split UTF-8 sequence: walk back over continuation bytes.
    for (int i = 0; i < 3 && !d.empty() &&
                    (static_cast<unsigned char>(d.back()) & 0xC0) == 0x80;
         ++i) {
      d.pop_back();
    }
  }
  proj->doc.auditLog.push_back({now, "user", action, object_id, d});
  proj->doc.project.modifiedAt = now;
  while (proj->doc.auditLog.size() > kMaxAuditEntries) {
    proj->doc.auditLog.erase(proj->doc.auditLog.begin());
  }
}

// ---------------------------------------------------------------------------
// UUID / time (uuid.cpp)
// ---------------------------------------------------------------------------
Uuid uuid_generate();
void now_iso8601_raw(char* buf, size_t cap);  // no-heap variant for sf_log

// ---------------------------------------------------------------------------
// Format checks (shared by validation + migration)
// ---------------------------------------------------------------------------
inline bool is_uuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    const char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  if (s[14] != '4') return false;  // version nibble
  const char v = s[19];
  return v == '8' || v == '9' || v == 'a' || v == 'b';  // variant
}

inline bool is_iso_timestamp(const std::string& s) {
  if (s.size() < 20) return false;
  auto digits = [&s](size_t off, size_t n) {
    for (size_t i = off; i < off + n; ++i)
      if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
  };
  if (!digits(0, 4) || s[4] != '-' || !digits(5, 2) || s[7] != '-' || !digits(8, 2) ||
      s[10] != 'T' || !digits(11, 2) || s[13] != ':' || !digits(14, 2) || s[16] != ':' ||
      !digits(17, 2))
    return false;
  if (std::stoi(s.substr(5, 2)) < 1 || std::stoi(s.substr(5, 2)) > 12) return false;
  if (std::stoi(s.substr(8, 2)) < 1 || std::stoi(s.substr(8, 2)) > 31) return false;
  if (std::stoi(s.substr(11, 2)) > 23 || std::stoi(s.substr(14, 2)) > 59 ||
      std::stoi(s.substr(17, 2)) > 60)
    return false;
  size_t p = 19;
  if (p < s.size() && s[p] == '.') {
    const size_t f = ++p;
    while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) ++p;
    if (p == f) return false;
  }
  if (p >= s.size()) return false;
  if (s[p] == 'Z') return p + 1 == s.size();
  if (s[p] == '+' || s[p] == '-') {
    if (s.size() != p + 6) return false;
    return digits(p + 1, 2) && s[p + 3] == ':' && digits(p + 4, 2);
  }
  return false;
}

inline bool looks_like_semver(const std::string& s) {
  size_t i = 0;
  for (int part = 0; part < 3; ++part) {
    const size_t start = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == start) return false;
    if (part < 2) {
      if (i >= s.size() || s[i] != '.') return false;
      ++i;
    }
  }
  return true;  // anything may follow the third component (suffix)
}

// ---------------------------------------------------------------------------
// UTF-8 name cap helper (G3 D5, PLAN_G3 §4.5)
// ---------------------------------------------------------------------------
// Count RFC 3629 code points in a NUL-terminated byte string. FAIL-OPEN:
// invalid UTF-8 never causes a rejection here — every malformed byte counts
// as one character (conservative: chars >= true code points, so a hostile/
// broken name can only be *over*-counted; the separate byte ceiling bounds
// the wire). Handles overlong encodings, lone continuation bytes, truncated
// sequences, surrogate halves, >U+10FFFF, and 1..4-byte sequences.
// Never reads past the terminating NUL.
// Serialization guarantee: accepted names may still contain malformed bytes;
// doc serialization uses error_handler_t::replace, so such bytes are emitted
// as U+FFFD replacement chars and never brick the document (G3 ORC-P1-2).
inline size_t utf8_char_count(const char* s) {
  if (!s) return 0;
  size_t count = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    const unsigned char c = *p;
    size_t seq;
    if (c < 0x80) {
      seq = 1;
    } else if ((c & 0xE0) == 0xC0) {
      seq = 2;
    } else if ((c & 0xF0) == 0xE0) {
      seq = 3;
    } else if ((c & 0xF8) == 0xF0) {
      seq = 4;
    } else {
      ++p; ++count;  // invalid lead (incl. lone continuation) -> 1 char
      continue;
    }
    if (seq == 1) {
      ++p; ++count;
      continue;
    }
    bool ok = true;
    for (size_t i = 1; i < seq; ++i) {
      if ((p[i] & 0xC0) != 0x80) {  // NUL terminates -> may stop here safely
        ok = false;
        break;
      }
    }
    if (ok) {
      unsigned int cp;
      if (seq == 2) {
        cp = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
        if (cp < 0x80) ok = false;  // overlong
      } else if (seq == 3) {
        cp = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        if (cp < 0x800) ok = false;                         // overlong
        else if (cp >= 0xD800 && cp <= 0xDFFF) ok = false;  // surrogate half
      } else {
        cp = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
             ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        if (cp < 0x10000) ok = false;               // overlong
        else if (cp > 0x10FFFF) ok = false;         // out of range
        else if (cp >= 0xD800 && cp <= 0xDFFF) ok = false;  // surrogate half
      }
    }
    if (!ok) {
      ++p; ++count;  // malformed byte -> 1 char; remaining bytes re-examined
      continue;
    }
    p += seq;
    ++count;
  }
  return count;
}

// ---------------------------------------------------------------------------
// G3 P7 — JSON depth pre-parse gate (PLAN_G3 §6 P7; SEC-G3-7/-G3-8)
// ---------------------------------------------------------------------------
// Pre-parse JSON nesting cap, enforced by scan_json_depth() BEFORE nlohmann
// parses on all 4 raw-JSON entry points (sf_project_from_json,
// sf_project_open_from_path, sf_validate_project_json, sf_migrate_json).
//
// Residual G3-1 — nlohmann fork BLOCKED, documented (do NOT implement): the
// vendored nlohmann 3.11.3 parser has NO nesting-depth limit (the upstream
// depth_limit parameter only landed in 3.12.0), so deeply nested documents
// recurse against the host stack — the exact exposure this gate bounds.
// Adopting a newer nlohmann would mean vendoring/patching a third-party
// dependency; no such patch is available in this environment, so the fork is
// BLOCKED and the iterative pre-parse scanner is the sanctioned mitigation
// (PLAN_G3 §10.1). Parser behavior itself is NEVER modified.
const int kMaxJsonDepth = 256;

// Verdicts of the depth scanner. SEC-G3-8 contract: the scanner ONLY ever
// pre-rejects with kDepthExceeded (structural nesting > kMaxJsonDepth) or
// kUnterminated (a string ran to end of input) — nothing else. Both are a
// subset of nlohmann's rejections (nlohmann always rejects unterminated
// strings; there is no nlohmann-accepted input that is >256 deep that this
// gate is not allowed to reject by design). Malformed-but-shallow input is
// deliberately left for nlohmann so its verdict is never changed by the
// scanner (malformed JSON still yields the nlohmann result).
enum class JsonScanStatus : int {
  kOk = 0,
  kDepthExceeded = 1,
  kUnterminated = 2,
};

// Strict ITERATIVE string/escape-aware structural-depth scanner (defined in
// schema.cpp). Walks the bytes ONE escape-char-at-a-time: tracks { } [ ]
// nesting outside strings only, skipping strings, escapes, \uXXXX units
// (4 chars consumed unconditionally — hex validity is nlohmann's call, see
// impl comment), raw control bytes, and braces/escapes inside strings.
// NEVER re-implements the JSON parser — a stray '}' on malformed input clamps
// at 0 instead of tripping. *max_depth_out receives the deepest nesting seen
// (0 for a bare scalar, 1 for a bare {}). O(n), zero per-char allocation.
JsonScanStatus scan_json_depth(const char* data, size_t len, int* max_depth_out);

// SEC-G3-7: shared pre-parse gate for ALL 4 raw-JSON entry points. Order:
//   (1) 8 MiB byte cap (kMaxDocBytes)  -> SF_E_FILE_TOO_LARGE,
//       "JSON input exceeds 8 MiB limit" (same text from_json used);
//   (2) scan_json_depth()              -> SF_E_SCHEMA, distinct texts
//       "schema: json depth exceeds 256" | "schema: unterminated string in json";
//   (3) nlohmann parse (unchanged parser; exceptions PROPAGATE to the caller's
//       existing try/catch — checked_parse never swallows parse errors).
// On pre-reject *out is untouched and the message is written to *err_out
// (may be NULL). Internal only — no public ABI addition.
sf_result_t checked_parse(const char* data, size_t len, json* out, std::string* err_out);

// ---------------------------------------------------------------------------
// JSON codec (json_codec.cpp)
// ---------------------------------------------------------------------------
json envelope_to_json(const ObjectEnvelope& e);
bool envelope_from_json(const json& j, ObjectEnvelope& out, std::string& err);
json audit_to_json(const AuditEntry& a);
bool audit_from_json(const json& j, AuditEntry& out, std::string& err);
json doc_to_json(const SfProjectDoc& d);
bool doc_from_json(const json& j, SfProjectDoc& out, std::string& err);
int peek_schema_version(const json& j);

// ---------------------------------------------------------------------------
// Structural validation (schema.cpp) — mirrors project_schema.json
// ---------------------------------------------------------------------------
bool validate_doc_json(const json& j, std::string& errOut);

// ---------------------------------------------------------------------------
// Migration core (migration.cpp) — in-memory path used by ABI + open()
// ---------------------------------------------------------------------------
sf_result_t migrate_doc_inplace(json& j, int32_t from_ver, int32_t to_ver, std::string& err);

// ---------------------------------------------------------------------------
// Error store + logging (diagnostics.cpp)
// ---------------------------------------------------------------------------
void set_last_error(const std::string& msg);              // thread-local
void set_global_last_error(const std::string& msg);
const char* thread_last_error_cstr();
const char* global_last_error_cstr();
void set_handle_error(SfProject* p, const std::string& msg);
void log_line(int level, const char* tag, const char* msg);

inline void set_handle_error(SfProject* p, const std::string& msg) {
  set_last_error(msg);
  if (p) p->lastError = msg;
}

// ---------------------------------------------------------------------------
// G3 P4b single-owner ABI boundary guards (PLAN_G3 §6 P4b; D3-amd SEC-G3-2/
// -G3-3/-G3-4 + oracle R-B(c) "reject, not contract").
//
// Every exported sf_* entry that touches `doc` takes exactly ONE acquire load
// of runnerState and, while the runner thread is alive (RUNNING/STOPPING),
// rejects: mutators report on the HANDLE error store (the existing per-entry
// pattern) and return SF_E_IO; reads report on the thread-local store and
// return SF_E_IO (or a safe empty value for the `const char*` getters). This
// is the same one-load/no-TOCTOU shape P4a used for sf_graph_apply_batch.
// Exactly two exemptions: sf_last_error (the runner NEVER writes lastError —
// SEC-G3-4) and sf_project_destroy (void-signature rule, SEC-G3-2).
constexpr const char* kRunnerBusyMsg = "project.busy: queue runner active";

// True while the runner thread is or may still be alive for this handle.
inline bool runner_alive(const SfProject* p) {
  return runner_thread_alive(p->runnerState.load(std::memory_order_acquire));
}

// Mutator guard — returns true when the call was rejected (handle error set).
inline bool runner_busy_mutator(SfProject* p, const char* tag) {
  if (!runner_alive(p)) return false;
  log_line(SF_LOG_WARN, tag, "busy: queue runner active");
  set_handle_error(p, kRunnerBusyMsg);
  return true;
}

// Read guard — returns true when the call was rejected (thread-local error set).
inline bool runner_busy_reader(const SfProject* p, const char* tag) {
  if (!runner_alive(p)) return false;
  log_line(SF_LOG_WARN, tag, "busy: queue runner active");
  set_last_error(kRunnerBusyMsg);
  return true;
}

// ---------------------------------------------------------------------------
// Command queue drain (command_queue.cpp / command_queue_thread.cpp)
// ---------------------------------------------------------------------------
// Internal apply entry — the G2 sf_graph_apply_batch loop WITHOUT any
// ownership/busy check (SEC-G3-1 split). The CALLER is the sanctioned
// mutation owner: the public sf_graph_apply_batch checks the single-owner
// guard (runner_thread_alive) and then forwards; the runner thread calls it
// directly on its own thread. Applies cmds[0..n) sequentially — same impl
// helpers, audit entries and modifiedAt bumps as the synchronous mutators.
// On the first failing cmd it stops, reports progress via *applied (count
// applied BEFORE the stop) and returns that cmd's code with its message in
// *err_out (may be NULL). Rejects SF_CMD_STOP (0) and SF_CMD_EVALUATE_MIXER
// (7) via the unsupported-type branch — the runner intercepts both before
// batching. NEVER touches the handle error store (SEC-G3-4: the runner calls
// this, and the runner never writes proj->lastError); the public entry copies
// *err_out into the caller's err_buf AND the handle error store.
sf_result_t apply_batch_impl(sf_project_t* p, const sf_cmd_t* cmds, size_t n,
                             size_t* applied, std::string* err_out);

// ---------------------------------------------------------------------------
// G4 P3 — runner publish observer (PLAN_G4 §4.1 D1). Internal C++ only: no
// public header, no export, no JNI, no schema/ABI change.
//
// The queue-runner thread is the project's mutation owner, so after each drain
// pass that applied >=1 mutation it may safely read proj->doc.signalGraph and
// hand a fresh RenderPlan to the observer. The observer belongs to the audio
// engine (P4), which owns the PlanSnapshotStore and knows its out_node_id; the
// runner just fires it.
// ---------------------------------------------------------------------------
class PlanSnapshotStore;  // snapshot.hpp (kept out of this header)

// Fired on the runner thread with the graph it just mutated. Implementations
// MUST be noexcept in effect: the snapshot store's publish() already contains
// compile throws (ORC-G4-03), and the runner additionally fences this call so
// nothing can escape the runner thread. A null `publish` means "no observer".
struct RunnerObserver {
  void* user = nullptr;  // opaque engine instance; never dereferenced here
  void (*publish)(void* user, const SignalGraphDoc& graph) = nullptr;
};

// Attaches (obs != NULL) or detaches (obs == NULL) the runner's observer.
// Caller-lifetime contract, enforced by the P4 engine's destroy ordering:
// attach BEFORE sf_queue_runner_start; detach AFTER sf_queue_runner_join (so no
// publish can be in flight when the engine/store is freed). The observer is
// copied by value into the runner. Defined in command_queue_thread.cpp.
void sf_queue_runner_set_observer(sf_queue_runner_t* r,
                                  const RunnerObserver* obs);

// ---------------------------------------------------------------------------
// ABI exception guard
// ---------------------------------------------------------------------------
}  // namespace sfcore

#define SF_CATCH_ERRORS()                                          \
  catch (const std::exception& e) {                                \
    ::sfcore::set_last_error(e.what());                            \
    return SF_E_SCHEMA;                                            \
  }                                                                \
  catch (...) {                                                    \
    ::sfcore::set_last_error("unknown native exception");          \
    return SF_E_SCHEMA;                                            \
  }
