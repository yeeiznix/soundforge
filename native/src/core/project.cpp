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
  if (p) delete reinterpret_cast<sfcore::SfProject*>(p);
}

extern "C" sf_result_t sf_project_clone(const sf_project_t* src, sf_project_t** out) {
  if (!src || !out) {
    sfcore::set_last_error("clone: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    *out = reinterpret_cast<sf_project_t*>(new sfcore::SfProject(*reinterpret_cast<const sfcore::SfProject*>(src)));
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
    const std::string s = sfcore::doc_to_json(proj->doc).dump(2);
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (!buf) {
      sfcore::set_last_error("to_json: out of memory");
      return SF_E_NOMEM;
    }
    std::memcpy(buf, s.c_str(), s.size() + 1);
    *out_json = buf;
    *out_len = s.size();
    return SF_OK;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_project_from_json(const char* json, size_t len, sf_project_t** out) {
  if (!json || !out) {
    sfcore::set_last_error("from_json: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    sfcore::json j = sfcore::json::parse(json, json + len);
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
    sfcore::SfProject* p = new sfcore::SfProject();
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
    const std::string now = sfcore::now_iso8601();
    proj->doc.auditLog.push_back({
        now,
        "system",
        "project.save",
        proj->doc.project.id,
        "engine " + proj->doc.engineVersion,
    });
    proj->doc.project.modifiedAt = now;
    const std::string s = sfcore::doc_to_json(proj->doc).dump(2);
    const sf_result_t rc = sfcore::write_file_atomic(path, s);
    if (rc == SF_OK) {
      sfcore::log_line(SF_LOG_INFO, "project", ("save: " + std::string(path)).c_str());
    }
    return rc;
  } SF_CATCH_ERRORS()
}

extern "C" sf_result_t sf_project_open_from_path(const char* path, sf_project_t** out) {
  if (!path || !out || !*path) {
    sfcore::set_last_error("open: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    std::string data;
    if (!sfcore::read_file(path, data)) {
      sfcore::set_last_error(std::string("open: cannot read '") + path + "'");
      return SF_E_NOT_FOUND;  // file missing vs read error — G0 collapses to NOT_FOUND
    }
    sfcore::json j = sfcore::json::parse(data);
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
    sfcore::SfProject* p = new sfcore::SfProject();
    p->doc = std::move(doc);
    *out = reinterpret_cast<sf_project_t*>(p);
    sfcore::log_line(SF_LOG_INFO, "project", ("open: " + std::string(path)).c_str());
    return SF_OK;
  } SF_CATCH_ERRORS()
}

// ---------------------------------------------------------------------------
// C ABI: Accessors
// ---------------------------------------------------------------------------
extern "C" const char* sf_project_get_name(const sf_project_t* p) {
  if (!p) return "";
  return reinterpret_cast<const sfcore::SfProject*>(p)->doc.project.name.c_str();
}

extern "C" const char* sf_project_get_id(const sf_project_t* p) {
  if (!p) return "";
  return reinterpret_cast<const sfcore::SfProject*>(p)->doc.project.id.c_str();
}

extern "C" int32_t sf_project_get_schema_version(const sf_project_t* p) {
  if (!p) return -1;
  return reinterpret_cast<const sfcore::SfProject*>(p)->doc.schemaVersion;
}

extern "C" const char* sf_project_get_engine_version(const sf_project_t* p) {
  if (!p) return "";
  return reinterpret_cast<const sfcore::SfProject*>(p)->doc.engineVersion.c_str();
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
namespace {

// Appends an actor-"user" audit entry and bumps project.modifiedAt.
// Mirrors save's system audit; detail is truncated to 512 chars as documented
// on AuditEntry. Caller must hold a non-null handle.
void user_audit(sfcore::SfProject* proj, const char* action,
                const sfcore::Uuid& object_id, const std::string& detail) {
  const std::string now = sfcore::now_iso8601();
  std::string d = detail;
  if (d.size() > 512) d.resize(512);
  proj->doc.auditLog.push_back({now, "user", action, object_id, d});
  proj->doc.project.modifiedAt = now;
}

}  // namespace

extern "C" sf_result_t sf_project_rename(sf_project_t* p, const char* new_name) {
  if (!p) {
    sfcore::set_handle_error(nullptr, "rename: null handle");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (!new_name || !*new_name) {
      sfcore::set_handle_error(proj, "rename: name must be non-empty");
      return SF_E_INVALID_ARG;
    }
    if (std::strlen(new_name) > 200) {
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
    if (!new_name || !*new_name) {
      sfcore::set_handle_error(proj, "venue.update: name must be non-empty");
      return SF_E_INVALID_ARG;
    }
    if (std::strlen(new_name) > 200) {
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
    if (sf_geo_validate_box(width_m, depth_m, height_m) != SF_OK) {
      sfcore::set_handle_error(proj, "venue.update: dimensions must be finite and > 0");
      return SF_E_INVALID_ARG;
    }
    proj->doc.venue.widthM = width_m;
    proj->doc.venue.depthM = depth_m;
    proj->doc.venue.heightM = height_m;
    // Keep scene geometry inside the (possibly smaller) room.
    auto clamp = [](double v, double hi) { return std::fmax(0.0, std::fmin(v, hi)); };
    proj->doc.scene.center = {clamp(proj->doc.scene.center.x, width_m),
                              clamp(proj->doc.scene.center.y, depth_m),
                              clamp(proj->doc.scene.center.z, height_m)};
    proj->doc.scene.listening = {clamp(proj->doc.scene.listening.x, width_m),
                                 clamp(proj->doc.scene.listening.y, depth_m),
                                 clamp(proj->doc.scene.listening.z, height_m)};
    user_audit(proj, "venue.update", proj->doc.venue.id,
               "dimensions=" + std::to_string(width_m) + "x" + std::to_string(depth_m) +
                   "x" + std::to_string(height_m));
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
      sfcore::set_handle_error(proj, "scene.update: venue dimensions invalid");
      return SF_E_SCHEMA;
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