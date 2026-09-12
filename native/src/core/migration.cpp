// SoundForge G1 — stepwise schema migration (PLAN_G0 §5.5, PLAN_G1 P2).
// Chain: 0→1→2, idempotent per step, injects defaults for missing canonical
// keys, ensures envelope shape, appends one project.migrate audit entry per
// step. The v1→v2 step adds venue.dimensions and scene.geometry with room
// defaults. Python mirror: python/soundforge_py/migrate.py (lockstep).
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

// Appends one project.migrate audit entry describing a completed step.
void append_migrate_audit(json& j, int32_t from, int32_t to) {
  json a = json::object();
  a["ts"] = now_iso8601();
  a["actor"] = "system";
  a["action"] = "project.migrate";
  a["objectId"] = j["project"]["id"];
  a["detail"] = "migrated " + std::to_string(from) + "->" + std::to_string(to);
  if (!j["auditLog"].is_array()) j["auditLog"] = json::array();
  j["auditLog"].push_back(a);
}

// One migration step: from version `from` to `from + 1`.
sf_result_t migrate_step(json& j, int32_t from, std::string& err) {
  const int32_t to = from + 1;
  if (peek_schema_version(j) >= to) return SF_OK;  // idempotent per step

  if (from == 0) {
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

    append_migrate_audit(j, 0, 1);
    return SF_OK;
  }

  if (from == 1) {
    j["schemaVersion"] = 2;
    j["engineVersion"] = sf_engine_version();

    // venue.dimensions — inject room defaults unless already positive.
    json& venue = j["venue"];
    if (!venue.is_object()) venue = json::object();
    double room[3] = {SF_ROOM_DEFAULT_W, SF_ROOM_DEFAULT_D, SF_ROOM_DEFAULT_H};
    if (!venue.contains("dimensions") || !venue["dimensions"].is_object()) {
      json d = json::object();
      d["widthM"] = SF_ROOM_DEFAULT_W;
      d["depthM"] = SF_ROOM_DEFAULT_D;
      d["heightM"] = SF_ROOM_DEFAULT_H;
      venue["dimensions"] = d;
    } else {
      const json& dims = venue["dimensions"];
      const char* keys[3] = {"widthM", "depthM", "heightM"};
      bool ok = true;
      for (int i = 0; i < 3; ++i) {
        if (!dims.contains(keys[i]) || !dims[keys[i]].is_number() ||
            dims[keys[i]].get<double>() <= 0.0) {
          ok = false;
          break;
        }
      }
      if (ok) {
        for (int i = 0; i < 3; ++i) room[i] = dims[keys[i]].get<double>();
      } else {
        json d = json::object();
        d["widthM"] = SF_ROOM_DEFAULT_W;
        d["depthM"] = SF_ROOM_DEFAULT_D;
        d["heightM"] = SF_ROOM_DEFAULT_H;
        venue["dimensions"] = d;
      }
    }

    // scene.geometry — center/listening at room middle unless already valid.
    auto room_mid_point = [&room]() {
      json p = json::object();
      p["x"] = room[0] / 2.0;
      p["y"] = room[1] / 2.0;
      p["z"] = room[2] / 2.0;
      return p;
    };
    json& scene = j["scene"];
    if (!scene.is_object()) scene = json::object();
    bool geo_ok = scene.contains("geometry") && scene["geometry"].is_object();
    if (geo_ok) {
      const json& geo = scene["geometry"];
      for (const char* pt : {"center", "listening"}) {
        if (!geo.contains(pt) || !geo[pt].is_object()) {
          geo_ok = false;
          break;
        }
        const json& p = geo[pt];
        for (const char* ax : {"x", "y", "z"}) {
          if (!p.contains(ax) || !p[ax].is_number()) {
            geo_ok = false;
            break;
          }
        }
        if (!geo_ok) break;
      }
    }
    if (!geo_ok) {
      json g = json::object();
      g["center"] = room_mid_point();
      g["listening"] = room_mid_point();
      scene["geometry"] = g;
    }

    append_migrate_audit(j, 1, 2);
    return SF_OK;
  }

  err = "no migration path from " + std::to_string(from) + " to " + std::to_string(to);
  return SF_E_VERSION;
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
  for (int32_t v = from_ver; v < to_ver; ++v) {
    const sf_result_t rc = migrate_step(j, v, err);
    if (rc != SF_OK) return rc;
  }
  return SF_OK;
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