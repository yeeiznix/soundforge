// SoundForge G0 — project handle lifecycle, JSON codec, atomic persistence,
/// health checks, and migration on open (§3.4, §5, §6.3, §9).
#include "sf_internal.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "soundforge/sf_project.h"
#include "soundforge/sf_diagnostics.h"
#include "soundforge/sf_geometry.h"

namespace sfcore {
namespace {

bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  out.resize(static_cast<size_t>(in.tellg()));
  in.seekg(0);
  in.read(out.data(), out.size());
  return true;
}

// Read with byte-cap enforced at input time (not post-parse).
// Returns false if file exceeds max_bytes; caller checks errOut.
bool read_file_capped(const std::string& path, size_t max_bytes, std::string& out, std::string& errOut) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    errOut = "cannot open";
    return false;
  }
  in.seekg(0, std::ios::end);
  size_t sz = static_cast<size_t>(in.tellg());
  if (sz > max_bytes) {
    errOut = "file too large";
    return false;  // do NOT populate out; caller checks result
  }
  out.resize(sz);
  in.seekg(0);
  in.read(out.data(), out.size());
  return true;
}

sf_result_t write_file_atomic(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    set_last_error("write: cannot open tmp");
    return SF_E_IO;
  }
  const size_t w = std::fwrite(data.data(), 1, data.size(), f);
  std::fflush(f);
  const int fd = fileno(f);
#ifdef _WIN32
  _commit(fd);
#else
  fsync(fd);
#endif
  std::fclose(f);
  if (w != data.size()) {
    std::remove(tmp.c_str());
    set_last_error("write: short write");
    return SF_E_IO;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    set_last_error("write: rename failed");
    return SF_E_IO;
  }
  return SF_OK;
}

bool copy_file(const std::string& src, const std::string& dst) {
  std::ifstream in(src, std::ios::binary);
  if (!in) return false;
  std::ofstream out(dst, std::ios::binary);
  if (!out) return false;
  out << in.rdbuf();
  return true;
}

}  // namespace

}  // namespace sfcore

// ---------------------------------------------------------------------------
// C ABI: Lifecycle
// ---------------------------------------------------------------------------
extern "C" sf_project_t* sf_project_create(const char* name, const char* author) {
  if (!name || !*name) {
    sfcore::set_last_error("project.create: name must be non-empty");
    return nullptr;
  }
  try {
    sfcore::SfProject* p = new (std::nothrow) sfcore::SfProject();
    if (!p) {
      sfcore::set_last_error("project.create: out of memory");
      return nullptr;
    }
    const std::string now = sfcore::now_iso8601();
    p->doc.schemaVersion = SF_SCHEMA_VERSION;
    p->doc.engineVersion = sf_engine_version();
    p->doc.project = {
        sfcore::uuid_generate(),
        name,
        now,
        now,
        author ? author : "",
        "",
    };
    p->doc.venue = {
        sfcore::uuid_generate(),
        "Untitled Venue",
        sfcore::SF_ROOM_DEFAULT_W,
        sfcore::SF_ROOM_DEFAULT_D,
        sfcore::SF_ROOM_DEFAULT_H,
    };
    const double cw = sfcore::SF_ROOM_DEFAULT_W / 2.0;
    const double cd = sfcore::SF_ROOM_DEFAULT_D / 2.0;
    const double ch = sfcore::SF_ROOM_DEFAULT_H / 2.0;
    p->doc.scene = {
        sfcore::uuid_generate(),
        "Default Scene",
        p->doc.venue.id,
        {cw, cd, ch},
        {cw, cd, ch},
    };
    p->doc.auditLog.push_back({
        now,
        "system",
        "project.create",
        p->doc.project.id,
        "schemaVersion " + std::to_string(SF_SCHEMA_VERSION) + " engine " + p->doc.engineVersion,
    });
    sfcore::log_line(SF_LOG_INFO, "project", ("create: " + std::string(name)).c_str());
    return reinterpret_cast<sf_project_t*>(p);
  } catch (const std::exception& e) {
    sfcore::set_last_error(e.what());
    return nullptr;
  } catch (...) {
    sfcore::set_last_error("unknown native exception");
    return nullptr;
  }
}

extern "C" void sf_project_destroy(sf_project_t* p) {
  if (!p) return;
  auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
  // P4b SEC-G3-2 (void-safe): while the runner thread is alive, freeing the
  // handle would dangle the runner's `proj`/`state` pointers (ORC-P4a-A). The
  // signature is additive-ABI `void`, so we set the handle error + log ERROR
  // and SKIP the free; the caller must stop+join, then destroy again.
  if (sfcore::runner_thread_alive(proj->runnerState.load(std::memory_order_acquire))) {
    sfcore::log_line(SF_LOG_ERROR, "project", "destroy: queue runner active");
    sfcore::set_handle_error(proj, "project.destroy: queue runner active");
    return;
  }
  delete proj;
}

extern "C" sf_result_t sf_project_clone(const sf_project_t* src, sf_project_t** out) {
  if (!src || !out) {
    sfcore::set_last_error("clone: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    const auto* sproj = reinterpret_cast<const sfcore::SfProject*>(src);
    // P4b (ORC-1): clone is a read — reject while RUNNING/STOPPING. The copy
    // ctor default-constructs runner state, so an IDLE clone is IDLE.
    if (runner_busy_reader(sproj, "project")) return SF_E_IO;
    *out = reinterpret_cast<sf_project_t*>(new sfcore::SfProject(*sproj));
    return SF_OK;
  } SF_CATCH_ERRORS()
}

// ---------------------------------------------------------------------------
// C ABI: JSON codec
// ---------------------------------------------------------------------------
extern "C" sf_result_t sf_project_to_json(const sf_project_t* p, char** out_json, size_t* out_len) {
  if (!p || !out_json || !out_len) {
    sfcore::set_last_error("to_json: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    const auto* proj = reinterpret_cast<const sfcore::SfProject*>(p);
    // P4b read guard (R-B(c)): reject while the runner owns the thread.
    if (runner_busy_reader(proj, "project")) return SF_E_IO;
    // error_handler_t::replace (G3 ORC-P1-1/2): accepted names may contain
    // malformed UTF-8 (utf8_char_count is fail-open); emit U+FFFD instead of
    // throwing, so serialization can never brick the document.
    const std::string s =
        sfcore::doc_to_json(proj->doc)
            .dump(2, ' ', false, sfcore::json::error_handler_t::replace);
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (!buf) {
      sfcore::set_last_error("to_json: out of memory");
      return SF_E_NOMEM;
    }
    std::memcpy(buf, s.c_str(), s.size() + 1);
    *out_json = buf;
    *out_len = s.size();
    return SF_OK;
  } catch (const std::exception& e) {
    auto* proj = const_cast<sfcore::SfProject*>(reinterpret_cast<const sfcore::SfProject*>(p));
    sfcore::set_handle_error(proj, e.what());  // ORC-P1-4: handle error too
    return SF_E_SCHEMA;
  } catch (...) {
    auto* proj = const_cast<sfcore::SfProject*>(reinterpret_cast<const sfcore::SfProject*>(p));
    sfcore::set_handle_error(proj, "unknown native exception");
    return SF_E_SCHEMA;
  }
}

extern "C" sf_result_t sf_project_from_json(const char* json, size_t len, sf_project_t** out) {
  if (!json || !out) {
    sfcore::set_last_error("from_json: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    // G3 P7 (SEC-G3-7): shared pre-parse gate (8 MiB byte cap + depth scan +
    // nlohmann) replaces the inline byte-cap check — same code and message,
    // depth gate added before any parse.
    std::string perr;
    sfcore::json j;
    const sf_result_t rc = sfcore::checked_parse(json, len, &j, &perr);
    if (rc != SF_OK) {
      sfcore::set_last_error(perr);
      return rc;
    }
    // Migration if needed.
    const int sv = sfcore::peek_schema_version(j);
    if (sv >= 0 && sv < SF_SCHEMA_VERSION) {
      std::string err;
      const sf_result_t rc = sfcore::migrate_doc_inplace(j, sv, SF_SCHEMA_VERSION, err);
      if (rc != SF_OK) return rc;
    } else if (sv > SF_SCHEMA_VERSION) {
      sfcore::set_last_error("from_json: unsupported schemaVersion " + std::to_string(sv));
      return SF_E_VERSION;
    }
    // Validate.
    std::string err;
    if (!sfcore::validate_doc_json(j, err)) {
      sfcore::set_last_error(err);
      return SF_E_SCHEMA;
    }
    sfcore::SfProjectDoc doc;
    if (!sfcore::doc_from_json(j, doc, err)) {
      sfcore::set_last_error(err);
      return SF_E_SCHEMA;
    }
    sfcore::SfProject* p = new (std::nothrow) sfcore::SfProject();
    if (!p) {
      sfcore::set_last_error("from_json: out of memory");
      return SF_E_NOMEM;
    }
    p->doc = std::move(doc);
    *out = reinterpret_cast<sf_project_t*>(p);
    return SF_OK;
  } SF_CATCH_ERRORS()
}

// ---------------------------------------------------------------------------
// C ABI: File I/O
// ---------------------------------------------------------------------------
extern "C" sf_result_t sf_project_save_to_path(const sf_project_t* p, const char* path) {
  if (!p || !path || !*path) {
    sfcore::set_last_error("save: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = const_cast<sfcore::SfProject*>(reinterpret_cast<const sfcore::SfProject*>(p));
    // P4b: save writes audit + modifiedAt (D3-amd) -> a MUTATOR; guard FIRST.
    if (runner_busy_mutator(proj, "project")) return SF_E_IO;
    const std::string now = sfcore::now_iso8601();
    proj->doc.auditLog.push_back({
        now,
        "system",
        "project.save",
        proj->doc.project.id,
        "engine " + proj->doc.engineVersion,
    });
    proj->doc.project.modifiedAt = now;
    // error_handler_t::replace (G3 ORC-P1-1/2): never throw on malformed UTF-8
    // in an accepted name; emit U+FFFD so save can never brick the document.
    const std::string s =
        sfcore::doc_to_json(proj->doc)
            .dump(2, ' ', false, sfcore::json::error_handler_t::replace);
    const sf_result_t rc = sfcore::write_file_atomic(path, s);
    if (rc == SF_OK) {
      sfcore::log_line(SF_LOG_INFO, "project", ("save: " + std::string(path)).c_str());
    }
    return rc;
  } catch (const std::exception& e) {
    auto* proj = const_cast<sfcore::SfProject*>(reinterpret_cast<const sfcore::SfProject*>(p));
    sfcore::set_handle_error(proj, e.what());  // ORC-P1-4: handle error too
    return SF_E_SCHEMA;
  } catch (...) {
    auto* proj = const_cast<sfcore::SfProject*>(reinterpret_cast<const sfcore::SfProject*>(p));
    sfcore::set_handle_error(proj, "unknown native exception");
    return SF_E_SCHEMA;
  }
}

extern "C" sf_result_t sf_project_open_from_path(const char* path, sf_project_t** out) {
  if (!path || !out || !*path) {
    sfcore::set_last_error("open: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    std::string data;
    std::string read_err;
    if (!sfcore::read_file_capped(path, sfcore::kMaxDocBytes, data, read_err)) {
      if (read_err == "file too large") {
        sfcore::set_last_error("file exceeds 8 MiB limit");
        return SF_E_FILE_TOO_LARGE;
      }
      sfcore::set_last_error(std::string("open: cannot read '") + path + "'");
      return SF_E_NOT_FOUND;  // file missing vs read error — G0 collapses to NOT_FOUND
    }
    // G3 P7 (SEC-G3-7): pre-parse gate (byte cap is already enforced by
    // read_file_capped above; depth scan + nlohmann here). from_json and
    // open_from_path share the same checked_parse.
    std::string perr;
    sfcore::json j;
    const sf_result_t prc = sfcore::checked_parse(data.data(), data.size(), &j, &perr);
    if (prc != SF_OK) {
      sfcore::set_last_error(perr);
      return prc;
    }
    // Peek schemaVersion (may be string or missing).
    int sv = sfcore::peek_schema_version(j);
    if (sv > SF_SCHEMA_VERSION) {
      sfcore::set_last_error("unsupported schemaVersion " + std::to_string(sv) + " (max " +
                             std::to_string(SF_SCHEMA_VERSION) + ")");
      return SF_E_VERSION;
    }
    if (sv >= 0 && sv < SF_SCHEMA_VERSION) {
      // Backup (best-effort) before migration.
      const std::string bak = std::string(path) + ".bak.v" + std::to_string(sv);
      sfcore::copy_file(path, bak);  // ignore failure, log warn
      std::string err;
      const sf_result_t rc = sfcore::migrate_doc_inplace(j, sv, SF_SCHEMA_VERSION, err);
      if (rc != SF_OK) {
        sfcore::log_line(SF_LOG_WARN, "migration", ("backup skipped for " + std::string(path)).c_str());
        return rc;
      }
    }
    // Validate.
    std::string err;
    if (!sfcore::validate_doc_json(j, err)) {
      sfcore::set_last_error(err);
      return SF_E_SCHEMA;
    }
    sfcore::SfProjectDoc doc;
    if (!sfcore::doc_from_json(j, doc, err)) {
      sfcore::set_last_error(err);
      return SF_E_SCHEMA;
    }
    sfcore::SfProject* p = new (std::nothrow) sfcore::SfProject();
    if (!p) {
      sfcore::set_last_error("open: out of memory");
      return SF_E_NOMEM;
    }
    p->doc = std::move(doc);
    *out = reinterpret_cast<sf_project_t*>(p);
    sfcore::log_line(SF_LOG_INFO, "project", ("open: " + std::string(path)).c_str());
    return SF_OK;
  } SF_CATCH_ERRORS()
}

// ---------------------------------------------------------------------------
// C ABI: Accessors
// ---------------------------------------------------------------------------
// P4b read guard on the getters (R-B(c)): these have no result channel, so a
// rejected read yields the same safe empty value a NULL handle yields and the
// thread-local error store carries "project.busy: queue runner active".
extern "C" const char* sf_project_get_name(const sf_project_t* p) {
  if (!p) return "";
  const auto* proj = reinterpret_cast<const sfcore::SfProject*>(p);
  if (runner_busy_reader(proj, "project")) return "";
  return proj->doc.project.name.c_str();
}

extern "C" const char* sf_project_get_id(const sf_project_t* p) {
  if (!p) return "";
  const auto* proj = reinterpret_cast<const sfcore::SfProject*>(p);
  if (runner_busy_reader(proj, "project")) return "";
  return proj->doc.project.id.c_str();
}

extern "C" int32_t sf_project_get_schema_version(const sf_project_t* p) {
  if (!p) return -1;
  const auto* proj = reinterpret_cast<const sfcore::SfProject*>(p);
  if (runner_busy_reader(proj, "project")) return -1;
  return proj->doc.schemaVersion;
}

extern "C" const char* sf_project_get_engine_version(const sf_project_t* p) {
  if (!p) return "";
  const auto* proj = reinterpret_cast<const sfcore::SfProject*>(p);
  if (runner_busy_reader(proj, "project")) return "";
  return proj->doc.engineVersion.c_str();
}

// ---------------------------------------------------------------------------
// C ABI: Health check
// ---------------------------------------------------------------------------
extern "C" sf_result_t sf_project_health_check(const sf_project_t* p, char* report_buf,
                                               size_t report_cap) {
  if (!p || !report_buf || report_cap == 0) {
    sfcore::set_last_error("health: invalid arguments");
    return SF_E_INVALID_ARG;
  }
  try {
    const auto* proj = reinterpret_cast<const sfcore::SfProject*>(p);
    // P4b read guard (R-B(c)): reject while the runner owns the thread.
    if (runner_busy_reader(proj, "project")) return SF_E_IO;
    const auto& doc = proj->doc;
    sfcore::json j = sfcore::doc_to_json(doc);
    sfcore::json report = sfcore::json::object();
    std::vector<std::string> warnings, errors;

    std::string verr;
    if (!sfcore::validate_doc_json(j, verr)) errors.push_back(verr);
    if (doc.schemaVersion != SF_SCHEMA_VERSION)
      errors.push_back("schemaVersion " + std::to_string(doc.schemaVersion) +
                       " (expected " + std::to_string(SF_SCHEMA_VERSION) + ")");

    // UUID uniqueness
    std::set<std::string> ids;
    std::vector<std::string> dups;
    auto add_id = [&](const std::string& id) {
      if (!id.empty() && !ids.insert(id).second) dups.push_back(id);
    };
    add_id(doc.project.id);
    add_id(doc.venue.id);
    add_id(doc.scene.id);
    for (const auto& e : doc.audienceReceivers) add_id(e.id);
    for (const auto& e : doc.equipment) add_id(e.id);
    for (const auto& e : doc.audioAssets) add_id(e.id);
    for (const auto& e : doc.dspPresets) add_id(e.id);
    for (const auto& e : doc.arrayConfigurations) add_id(e.id);
    for (const auto& e : doc.measurements) add_id(e.id);
    for (const auto& e : doc.simulationRuns) add_id(e.id);
    for (const auto& e : doc.trainingScenarios) add_id(e.id);
    for (const auto& e : doc.inventoryRefs) add_id(e.id);
    for (const auto& e : doc.reports) add_id(e.id);
    for (const auto& d : dups) errors.push_back("duplicate object id " + d);

    // AuditLog order
    for (size_t i = 1; i < doc.auditLog.size(); ++i) {
      if (doc.auditLog[i - 1].ts > doc.auditLog[i].ts) {
        warnings.push_back("auditLog not sorted by ts ascending (auto-sorted on save)");
        break;
      }
    }

    // G1: legacy or hand-edited docs may carry scene geometry outside the
    // venue box. The schema validator is structural only, so such docs still
    // open; surface a Warning (not Error) so they remain openable and can be
    // repaired in the editor (PLAN_G1 §9.6). Mutators reject out-of-bounds
    // writes, so this only fires for pre-existing documents.
    if (sf_geo_validate_box(doc.venue.widthM, doc.venue.depthM, doc.venue.heightM) == SF_OK) {
      int c_in = 0, l_in = 0;
      const sf_result_t rc1 =
          sf_geo_point_in_box(doc.scene.center.x, doc.scene.center.y, doc.scene.center.z,
                              doc.venue.widthM, doc.venue.depthM, doc.venue.heightM, &c_in);
      const sf_result_t rc2 =
          sf_geo_point_in_box(doc.scene.listening.x, doc.scene.listening.y,
                              doc.scene.listening.z, doc.venue.widthM, doc.venue.depthM,
                              doc.venue.heightM, &l_in);
      if (rc1 != SF_OK || rc2 != SF_OK || !c_in || !l_in) {
        warnings.push_back("scene geometry outside venue bounds (legacy doc)");
      }
    }

    // G2: signal graph health (§3.3) — dangling edges and cycles are errors;
    // nodes unreachable from any source are warnings (repairable in editor).
    {
      const auto& g = doc.signalGraph;
      std::set<std::string> node_ids;
      for (const auto& n : g.nodes) node_ids.insert(n.id);
      for (const auto& e : g.edges) {
        if (!node_ids.count(e.fromNodeId)) {
          errors.push_back("dangling graph edge from " + e.fromNodeId);
        }
        if (!node_ids.count(e.toNodeId)) {
          errors.push_back("dangling graph edge to " + e.toNodeId);
        }
      }
      // SEC-G3-9 (D6-amd): every non-empty dspPresetRef must resolve to an
      // existing dspPresets[].id. ""/null = none — never an error.
      std::set<std::string> preset_ids;
      for (const auto& env : doc.dspPresets) preset_ids.insert(env.id);
      for (const auto& n : g.nodes) {
        if (!n.dspPresetRef.empty() && !preset_ids.count(n.dspPresetRef)) {
          errors.push_back("dangling graph dspPresetRef " + n.dspPresetRef + " on node " + n.id);
        }
      }
      std::vector<std::string> order;
      std::string topo_err;
      if (!sfcore::topological_order(g, order, topo_err)) {
        errors.push_back("signal graph cycle: " + topo_err);
      }
      for (const auto& n : g.nodes) {
        if (n.kind == sfcore::SfNodeSource) continue;
        bool reachable = false;
        for (const auto& s : g.nodes) {
          if (s.kind == sfcore::SfNodeSource && s.id != n.id &&
              sfcore::can_reach(g, s.id, n.id)) {
            reachable = true;
            break;
          }
        }
        if (!reachable) {
          warnings.push_back("signal graph node not reachable from any source: " + n.id);
        }
      }
    }

    // Stats
    sfcore::json stats = sfcore::json::object();
    stats["audienceReceivers"] = doc.audienceReceivers.size();
    stats["equipment"] = doc.equipment.size();
    stats["audioAssets"] = doc.audioAssets.size();
    stats["dspPresets"] = doc.dspPresets.size();
    stats["arrayConfigurations"] = doc.arrayConfigurations.size();
    stats["measurements"] = doc.measurements.size();
    stats["simulationRuns"] = doc.simulationRuns.size();
    stats["trainingScenarios"] = doc.trainingScenarios.size();
    stats["inventoryRefs"] = doc.inventoryRefs.size();
    stats["reports"] = doc.reports.size();
    stats["auditLog"] = doc.auditLog.size();
    stats["signalGraphNodes"] = doc.signalGraph.nodes.size();
    stats["signalGraphEdges"] = doc.signalGraph.edges.size();

    report["status"] = errors.empty() ? (warnings.empty() ? "ok" : "warning") : "error";
    report["warnings"] = warnings;
    report["errors"] = errors;
    report["stats"] = stats;

    const std::string s = report.dump(2);
    if (s.size() + 1 > report_cap) {
      sfcore::set_last_error("health: report buffer too small");
      return SF_E_NOMEM;
    }
    std::memcpy(report_buf, s.c_str(), s.size() + 1);
    if (report["status"] == "ok") {
      sfcore::log_line(SF_LOG_INFO, "health", "project ok");
      return SF_OK;
    }
    sfcore::log_line(SF_LOG_WARN, "health", s.c_str());
    return SF_E_SCHEMA;
  } SF_CATCH_ERRORS()
}

// ---------------------------------------------------------------------------
// C ABI: editing mutators (G1, PLAN_G1 §4.2)
// ---------------------------------------------------------------------------
extern "C" sf_result_t sf_project_rename(sf_project_t* p, const char* new_name) {
  if (!p) {
    sfcore::set_handle_error(nullptr, "rename: null handle");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "project")) return SF_E_IO;  // P4b single-owner
    if (!new_name || !*new_name) {
      sfcore::set_handle_error(proj, "rename: name must be non-empty");
      return SF_E_INVALID_ARG;
    }
    if (std::strlen(new_name) > 800 || sfcore::utf8_char_count(new_name) > 200) {
      sfcore::set_handle_error(proj, "rename: name too long (>200 chars)");
      return SF_E_INVALID_ARG;
    }
    proj->doc.project.name = new_name;
    user_audit(proj, "project.rename", proj->doc.project.id,
               "name=" + std::string(new_name));
    sfcore::log_line(SF_LOG_INFO, "project", ("rename: " + std::string(new_name)).c_str());
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_venue_rename(sf_project_t* p, const char* new_name) {
  if (!p) {
    sfcore::set_handle_error(nullptr, "venue.update: null handle");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "project")) return SF_E_IO;  // P4b single-owner
    if (!new_name || !*new_name) {
      sfcore::set_handle_error(proj, "venue.update: name must be non-empty");
      return SF_E_INVALID_ARG;
    }
    if (std::strlen(new_name) > 800 || sfcore::utf8_char_count(new_name) > 200) {
      sfcore::set_handle_error(proj, "venue.update: name too long (>200 chars)");
      return SF_E_INVALID_ARG;
    }
    proj->doc.venue.name = new_name;
    user_audit(proj, "venue.update", proj->doc.venue.id,
               "name=" + std::string(new_name));
    sfcore::log_line(SF_LOG_INFO, "project", "venue.update: name");
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_venue_set_dimensions(sf_project_t* p, double width_m,
                                               double depth_m, double height_m) {
  if (!p) {
    sfcore::set_handle_error(nullptr, "venue.update: null handle");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "project")) return SF_E_IO;  // P4b single-owner
    if (sf_geo_validate_box(width_m, depth_m, height_m) != SF_OK) {
      sfcore::set_handle_error(proj, "venue.update: dimensions must be finite and > 0");
      return SF_E_INVALID_ARG;
    }
    proj->doc.venue.widthM = width_m;
    proj->doc.venue.depthM = depth_m;
    proj->doc.venue.heightM = height_m;
    // Keep scene geometry inside the (possibly smaller) room.
    const sfcore::Point3 old_center = proj->doc.scene.center;
    const sfcore::Point3 old_listening = proj->doc.scene.listening;
    auto clamp = [](double v, double hi) { return std::fmax(0.0, std::fmin(v, hi)); };
    proj->doc.scene.center = {clamp(proj->doc.scene.center.x, width_m),
                              clamp(proj->doc.scene.center.y, depth_m),
                              clamp(proj->doc.scene.center.z, height_m)};
    proj->doc.scene.listening = {clamp(proj->doc.scene.listening.x, width_m),
                                 clamp(proj->doc.scene.listening.y, depth_m),
                                 clamp(proj->doc.scene.listening.z, height_m)};
    const bool snapped =
        old_center.x != proj->doc.scene.center.x ||
        old_center.y != proj->doc.scene.center.y ||
        old_center.z != proj->doc.scene.center.z ||
        old_listening.x != proj->doc.scene.listening.x ||
        old_listening.y != proj->doc.scene.listening.y ||
        old_listening.z != proj->doc.scene.listening.z;
    std::string detail = "dimensions=" + std::to_string(width_m) + "x" +
                         std::to_string(depth_m) + "x" + std::to_string(height_m);
    if (snapped) detail += "; scene geometry snapped to room";
    user_audit(proj, "venue.update", proj->doc.venue.id, detail);
    sfcore::log_line(SF_LOG_INFO, "project", "venue.update: dimensions");
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_scene_set_geometry(sf_project_t* p, double cx, double cy,
                                             double cz, double lx, double ly, double lz) {
  if (!p) {
    sfcore::set_handle_error(nullptr, "scene.update: null handle");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "project")) return SF_E_IO;  // P4b single-owner
    const double w = proj->doc.venue.widthM;
    const double d = proj->doc.venue.depthM;
    const double h = proj->doc.venue.heightM;
    if (sf_geo_validate_box(w, d, h) != SF_OK) {
      sfcore::set_handle_error(proj, "scene.update: venue dimensions missing or invalid");
      return SF_E_SCHEMA;
    }
    int c_in = 0, l_in = 0;
    const sf_result_t rc1 = sf_geo_point_in_box(cx, cy, cz, w, d, h, &c_in);
    const sf_result_t rc2 = sf_geo_point_in_box(lx, ly, lz, w, d, h, &l_in);
    if (rc1 != SF_OK || rc2 != SF_OK) {
      sfcore::set_handle_error(proj, "scene.update: coordinates must be finite");
      return SF_E_INVALID_ARG;
    }
    if (!c_in || !l_in) {
      sfcore::set_handle_error(proj, "scene.update: geometry outside venue bounds");
      return SF_E_SCHEMA;
    }
    proj->doc.scene.center = {cx, cy, cz};
    proj->doc.scene.listening = {lx, ly, lz};
    user_audit(proj, "scene.update", proj->doc.scene.id,
               "center=" + std::to_string(cx) + "," + std::to_string(cy) + "," +
                   std::to_string(cz) + " listening=" + std::to_string(lx) + "," +
                   std::to_string(ly) + "," + std::to_string(lz));
    sfcore::log_line(SF_LOG_INFO, "project", "scene.update: geometry");
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_scene_rename(sf_project_t* p, const char* new_name) {
  if (!p) {
    sfcore::set_handle_error(nullptr, "scene.update: null handle");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "project")) return SF_E_IO;  // P4b single-owner
    if (!new_name || !*new_name) {
      sfcore::set_handle_error(proj, "scene.update: name must be non-empty");
      return SF_E_INVALID_ARG;
    }
    // Reject whitespace-only names (non-empty but semantically blank).
    bool blank = true;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(new_name); *q; ++q) {
      if (!std::isspace(*q)) {
        blank = false;
        break;
      }
    }
    if (blank) {
      sfcore::set_handle_error(proj, "scene.update: name must be non-empty");
      return SF_E_INVALID_ARG;
    }
    // <= 200 Unicode code points AND <= 800 UTF-8 bytes (D5).
    if (std::strlen(new_name) > 800 || sfcore::utf8_char_count(new_name) > 200) {
      sfcore::set_handle_error(proj, "scene.update: name too long (>200 chars)");
      return SF_E_INVALID_ARG;
    }
    proj->doc.scene.name = new_name;  // ONLY field touched (freeze invariant)
    user_audit(proj, "scene.update", proj->doc.scene.id,
               "rename name=" + std::string(new_name));
    sfcore::log_line(SF_LOG_INFO, "project", "scene.update: rename");
    return SF_OK;
  } SF_CATCH_ERRORS()
}