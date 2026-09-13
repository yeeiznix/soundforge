// Internal C++ core shared by native/src/core/*.cpp — NOT part of the public ABI.
// Canonical project data model (§9), codec, validation, migration, error store.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
struct SfProject {
  SfProjectDoc doc;
  std::string lastError = "no error";
};

// ---------------------------------------------------------------------------
// UUID / time (uuid.cpp)
// ---------------------------------------------------------------------------
Uuid uuid_generate();
IsoTimestamp now_iso8601();
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
