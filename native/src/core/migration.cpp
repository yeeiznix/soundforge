// SoundForge G0 — schema migration (§5.5).
// G0 path: 0→1, idempotent, injects defaults for missing canonical keys,
// ensures envelope shape, appends a project.migrate audit entry.
#include "sf_internal.hpp"

#include <cstring>

namespace sfcore {
namespace {

json default_value_for_key(const std::string& key) {
  if (key == "signalGraph" || key == "powerGraph") return empty_graph();
  if (key == "venue") {
    json v = json::object();
    v["id"] = uuid_generate();
    v["name"] = "Untitled Venue";
    return v;
  }
  if (key == "scene") {
    json s = json::object();
    s["id"] = uuid_generate();
    s["name"] = "Default Scene";
    s["venueRef"] = "";  // wired to venue below
    return s;
  }
  return json::array();  // collections + auditLog
}

const char* singular_for(const std::string& key) {
  for (const auto& c : collections()) {
    if (key == c.key) return c.singular;
  }
  return "equipment";
}

}  // namespace

sf_result_t migrate_doc_inplace(json& j, int32_t from_ver, int32_t to_ver, std::string& err) {
  if (!j.is_object()) {
    err = "root: expected JSON object";
    return SF_E_SCHEMA;
  }
  if (from_ver == to_ver) return SF_OK;  // no-op
  if (to_ver < from_ver) {
    err = "downgrade not supported";
    return SF_E_VERSION;
  }

  if (from_ver == 0 && to_ver == 1) {
    const int current = peek_schema_version(j);
    if (current >= 1) return SF_OK;  // idempotent — already migrated

    j["schemaVersion"] = 1;
    j["engineVersion"] = sf_engine_version();  // engine that performed migration

    // Project placeholder if absent/broken.
    if (!j.contains("project") || !j["project"].is_object()) {
      const std::string now = now_iso8601();
      j["project"] = json{
          {"id", uuid_generate()},      {"name", "Migrated Project"},
          {"createdAt", now},           {"modifiedAt", now},
          {"author", ""},               {"notes", ""},
      };
    } else {
      json& pr = j["project"];
      if (!pr.contains("id") || !is_uuid(pr.value("id", ""))) pr["id"] = uuid_generate();
      if (!pr.contains("name") || pr.value("name", "").empty()) pr["name"] = "Migrated Project";
      for (const char* ts : {"createdAt", "modifiedAt"}) {
        if (!pr.contains(ts) || !pr[ts].is_string() ||
            !is_iso_timestamp(pr[ts].get<std::string>())) {
          pr[ts] = now_iso8601();
        }
      }
      for (const char* s : {"author", "notes"}) {
        if (!pr.contains(s) || !pr[s].is_string()) pr[s] = "";
      }
    }

    // Inject missing canonical keys.
    for (const auto& k : top_level_keys()) {
      if (!j.contains(k)) j[k] = default_value_for_key(k);
    }

    // Ensure graph shape.
    for (const char* gk : {"signalGraph", "powerGraph"}) {
      json& g = j[gk];
      if (!g.is_object()) g = empty_graph();
      if (!g.contains("nodes") || !g["nodes"].is_array()) g["nodes"] = json::array();
      if (!g.contains("edges") || !g["edges"].is_array()) g["edges"] = json::array();
    }

    // Ensure envelope shape for every collection element.
    for (const auto& c : collections()) {
      json& arr = j[c.key];
      if (!arr.is_array()) arr = json::array();
      for (auto& e : arr) {
        if (!e.is_object()) e = json::object();
        if (!e.contains("id") || !is_uuid(e.value("id", ""))) e["id"] = uuid_generate();
        if (!e.contains("type") || !e["type"].is_string() || e["type"].get<std::string>().empty())
          e["type"] = c.singular;
        if (!e.contains("version") || !e["version"].is_number_integer() ||
            e["version"].get<int>() < 1)
          e["version"] = 1;
        for (const char* ts : {"createdAt", "modifiedAt"}) {
          if (!e.contains(ts) || !e[ts].is_string() ||
              !is_iso_timestamp(e[ts].get<std::string>())) {
            e[ts] = now_iso8601();
          }
        }
        if (!e.contains("provenance") || !e["provenance"].is_string() ||
            e["provenance"].get<std::string>().empty()) {
          e["provenance"] = "migrated:v0->v1";
        }
        if (!e.contains("data") || !e["data"].is_object()) e["data"] = json::object();
      }
    }

    // Wire scene.venueRef to venue id.
    json& venue = j["venue"];
    if (!venue.contains("id") || !is_uuid(venue.value("id", ""))) venue["id"] = uuid_generate();
    json& scene = j["scene"];
    if (!scene.contains("id") || !is_uuid(scene.value("id", ""))) scene["id"] = uuid_generate();
    if (!scene.contains("venueRef") || !is_uuid(scene.value("venueRef", "")))
      scene["venueRef"] = venue["id"];

    // Audit entry.
    json a = json::object();
    a["ts"] = now_iso8601();
    a["actor"] = "system";
    a["action"] = "project.migrate";
    a["objectId"] = j["project"]["id"];
    a["detail"] = "migrated 0->1";
    if (!j["auditLog"].is_array()) j["auditLog"] = json::array();
    j["auditLog"].push_back(a);

    return SF_OK;
  }

  err = "no migration path from " + std::to_string(from_ver) + " to " + std::to_string(to_ver);
  return SF_E_VERSION;
}

}  // namespace sfcore

extern "C" sf_result_t sf_migrate_json(char* json_inout, size_t* inout_len, size_t cap,
                                       int32_t from_ver, int32_t to_ver) {
  if (!json_inout || !inout_len) {
    sfcore::set_last_error("migrate: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    const std::string in(json_inout, *inout_len);
    sfcore::json j = sfcore::json::parse(in);
    std::string err;
    const sf_result_t rc = sfcore::migrate_doc_inplace(j, from_ver, to_ver, err);
    if (rc != SF_OK) {
      sfcore::set_last_error(err.empty() ? "migration failed" : err);
      return rc;
    }
    const std::string out = j.dump(2);
    if (out.size() + 1 > cap) {
      sfcore::set_last_error("migrate: output buffer too small");
      return SF_E_NOMEM;
    }
    std::memcpy(json_inout, out.c_str(), out.size() + 1);
    *inout_len = out.size();
    return SF_OK;
  } catch (const std::exception& e) {
    sfcore::set_last_error(std::string("migrate: ") + e.what());
    return SF_E_SCHEMA;
  } catch (...) {
    sfcore::set_last_error("migrate: unknown exception");
    return SF_E_SCHEMA;
  }
}
