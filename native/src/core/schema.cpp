// SoundForge G0 — structural validation of project JSON (§3.3, §7.1).
// Mirrors native/data/schemas/project_schema.json (Draft 2020-12) structurally:
// required keys, types, UUID v4 format, RFC3339 timestamps, closed key set.
#include "sf_internal.hpp"

#include <string_view>

namespace sfcore {

bool validate_doc_json(const json& j, std::string& errOut) {
  std::vector<std::string> errs;
  auto add = [&errs](std::string m) { errs.push_back(std::move(m)); };

  if (!j.is_object()) {
    errOut = "root: expected JSON object";
    return false;
  }

  const auto& kKeys = top_level_keys();
  const std::set<std::string> allowed(kKeys.begin(), kKeys.end());
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!allowed.count(it.key())) add("unknown top-level key '" + it.key() + "'");
  }
  for (const auto& k : kKeys) {
    if (!j.contains(k)) add("missing required key '" + k + "'");
  }

  auto check_str = [&](const json& obj, const char* field, const char* ctx, bool nonEmpty) {
    if (!obj.contains(field)) {
      add(std::string(ctx) + ": missing '" + field + "'");
      return;
    }
    const auto& v = obj[field];
    if (!v.is_string()) {
      add(std::string(ctx) + ": '" + field + "' must be a string");
      return;
    }
    if (nonEmpty && v.get<std::string>().empty()) {
      add(std::string(ctx) + ": '" + field + "' must be non-empty");
    }
  };
  auto check_uuid = [&](const json& obj, const char* field, const char* ctx) {
    if (!obj.contains(field)) {
      add(std::string(ctx) + ": missing '" + field + "'");
      return;
    }
    const auto& v = obj[field];
    if (!v.is_string()) {
      add(std::string(ctx) + ": '" + field + "' must be a string");
      return;
    }
    if (!is_uuid(v.get<std::string>()))
      add(std::string(ctx) + ": invalid uuid '" + v.get<std::string>() + "'");
  };
  auto check_ts = [&](const json& obj, const char* field, const char* ctx) {
    if (!obj.contains(field)) {
      add(std::string(ctx) + ": missing '" + field + "'");
      return;
    }
    const auto& v = obj[field];
    if (!v.is_string()) {
      add(std::string(ctx) + ": '" + field + "' must be a string");
      return;
    }
    if (!is_iso_timestamp(v.get<std::string>()))
      add(std::string(ctx) + ": invalid timestamp '" + v.get<std::string>() + "'");
  };

  // schemaVersion / engineVersion
  if (j.contains("schemaVersion")) {
    const auto& sv = j["schemaVersion"];
    if (!sv.is_number_integer()) {
      add("schemaVersion: expected integer");
    } else if (sv.get<int>() != SF_SCHEMA_VERSION) {
      add("schemaVersion: unsupported value " + std::to_string(sv.get<int>()) + " (expected " +
          std::to_string(SF_SCHEMA_VERSION) + ")");
    }
  }
  if (j.contains("engineVersion")) {
    const auto& ev = j["engineVersion"];
    if (!ev.is_string()) {
      add("engineVersion: expected string");
    } else if (!looks_like_semver(ev.get<std::string>())) {
      add("engineVersion: not semver '" + ev.get<std::string>() + "'");
    }
  }

  // project
  if (j.contains("project")) {
    const json& pr = j["project"];
    if (!pr.is_object()) {
      add("project: expected object");
    } else {
      check_uuid(pr, "id", "project");
      check_str(pr, "name", "project", true);
      check_ts(pr, "createdAt", "project");
      check_ts(pr, "modifiedAt", "project");
      check_str(pr, "author", "project", false);
      check_str(pr, "notes", "project", false);
    }
  }

  // venue / scene (schema v2: venue.dimensions, scene.geometry)
  auto check_point3 = [&](const json& o, const char* ctx) {
    if (!o.is_object()) {
      add(std::string(ctx) + ": expected object");
      return;
    }
    for (const char* ax : {"x", "y", "z"}) {
      if (!o.contains(ax)) {
        add(std::string(ctx) + ": missing '" + ax + "'");
      } else if (!o[ax].is_number()) {
        add(std::string(ctx) + ": '" + ax + "' must be a number");
      }
    }
  };
  auto check_positive = [&](const json& o, const char* field, const char* ctx) {
    if (!o.contains(field)) {
      add(std::string(ctx) + ": missing '" + field + "'");
    } else if (!o[field].is_number()) {
      add(std::string(ctx) + ": '" + field + "' must be a number");
    } else if (o[field].get<double>() <= 0.0) {
      add(std::string(ctx) + ": '" + field + "' must be > 0");
    }
  };
  if (j.contains("venue")) {
    const json& v = j["venue"];
    if (!v.is_object()) {
      add("venue: expected object");
    } else {
      check_uuid(v, "id", "venue");
      check_str(v, "name", "venue", false);
      if (!v.contains("dimensions")) {
        add("venue: missing 'dimensions'");
      } else if (!v["dimensions"].is_object()) {
        add("venue.dimensions: expected object");
      } else {
        check_positive(v["dimensions"], "widthM", "venue.dimensions");
        check_positive(v["dimensions"], "depthM", "venue.dimensions");
        check_positive(v["dimensions"], "heightM", "venue.dimensions");
      }
    }
  }
  if (j.contains("scene")) {
    const json& s = j["scene"];
    if (!s.is_object()) {
      add("scene: expected object");
    } else {
      check_uuid(s, "id", "scene");
      check_str(s, "name", "scene", false);
      check_uuid(s, "venueRef", "scene");
      if (!s.contains("geometry")) {
        add("scene: missing 'geometry'");
      } else if (!s["geometry"].is_object()) {
        add("scene.geometry: expected object");
      } else {
        const json& g = s["geometry"];
        for (const char* pt : {"center", "listening"}) {
          if (!g.contains(pt)) {
            add(std::string("scene.geometry: missing '") + pt + "'");
          } else {
            check_point3(g[pt], (std::string("scene.geometry.") + pt).c_str());
          }
        }
      }
    }
  }

  // graphs
  auto check_graph = [&](const json& g, const char* ctx) {
    if (!g.is_object()) {
      add(std::string(ctx) + ": expected object");
      return;
    }
    for (const char* f : {"nodes", "edges"}) {
      if (!g.contains(f)) {
        add(std::string(ctx) + ": missing '" + f + "'");
      } else if (!g[f].is_array()) {
        add(std::string(ctx) + "." + f + ": must be an array");
      }
    }
  };
  if (j.contains("signalGraph")) check_graph(j["signalGraph"], "signalGraph");
  if (j.contains("powerGraph")) check_graph(j["powerGraph"], "powerGraph");

  // collections of envelopes
  for (const auto& c : collections()) {
    if (!j.contains(c.key)) continue;
    const json& arr = j[c.key];
    if (!arr.is_array()) {
      add(std::string(c.key) + ": expected array");
      continue;
    }
    int idx = 0;
    for (const auto& e : arr) {
      const std::string ctx = std::string(c.key) + "[" + std::to_string(idx) + "]";
      if (!e.is_object()) {
        add(ctx + ": expected object");
        ++idx;
        continue;
      }
      check_uuid(e, "id", ctx.c_str());
      if (!e.contains("type")) {
        add(ctx + ": missing 'type'");
      } else if (!e["type"].is_string()) {
        add(ctx + ": 'type' must be a string");
      } else {
        bool ok = false;
        for (const auto& ci : collections()) {
          if (std::string_view(ci.singular) == e["type"].get_ref<const std::string&>()) {
            ok = true;
            break;
          }
        }
        if (!ok) add(ctx + ": invalid type '" + e["type"].get<std::string>() + "'");
      }
      if (!e.contains("version")) {
        add(ctx + ": missing 'version'");
      } else if (!e["version"].is_number_integer() || e["version"].get<int>() < 1) {
        add(ctx + ": version must be a positive integer");
      }
      check_ts(e, "createdAt", ctx.c_str());
      check_ts(e, "modifiedAt", ctx.c_str());
      check_str(e, "provenance", ctx.c_str(), true);
      if (!e.contains("data")) {
        add(ctx + ": missing 'data'");
      } else if (!e["data"].is_object()) {
        add(ctx + ": data must be an object");
      }
      ++idx;
    }
  }

  // auditLog
  if (j.contains("auditLog")) {
    const json& arr = j["auditLog"];
    if (!arr.is_array()) {
      add("auditLog: expected array");
    } else {
      int idx = 0;
      for (const auto& a : arr) {
        const std::string ctx = "auditLog[" + std::to_string(idx) + "]";
        if (!a.is_object()) {
          add(ctx + ": expected object");
          ++idx;
          continue;
        }
        check_ts(a, "ts", ctx.c_str());
        check_str(a, "actor", ctx.c_str(), true);
        if (!a.contains("action")) {
          add(ctx + ": missing 'action'");
        } else if (!a["action"].is_string()) {
          add(ctx + ": 'action' must be a string");
        } else {
          bool ok = false;
          const std::string act = a["action"].get<std::string>();
          for (const char* k : audit_actions()) {
            if (act == k) {
              ok = true;
              break;
            }
          }
          if (!ok) add(ctx + ": unknown action '" + act + "'");
        }
        check_uuid(a, "objectId", ctx.c_str());
        check_str(a, "detail", ctx.c_str(), false);
        ++idx;
      }
    }
  }

  if (errs.empty()) {
    errOut.clear();
    return true;
  }
  std::string joined;
  for (size_t i = 0; i < errs.size(); ++i) {
    if (i) joined += "; ";
    joined += errs[i];
  }
  if (joined.size() > 1024) {
    joined.resize(1021);
    joined += "...";
  }
  errOut = std::to_string(errs.size()) + " error(s): " + joined;
  return false;
}

}  // namespace sfcore

extern "C" sf_result_t sf_validate_project_json(const char* json, size_t len, char* err_buf,
                                                size_t err_cap) {
  if (!json || !err_buf || err_cap == 0) {
    sfcore::set_last_error("validate: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    sfcore::json j = sfcore::json::parse(json, json + len);
    std::string err;
    if (!sfcore::validate_doc_json(j, err)) {
      if (!err.empty()) {
        sfcore::set_last_error(err);
        std::strncpy(err_buf, err.c_str(), err_cap - 1);
        err_buf[err_cap - 1] = '\0';
      }
      return SF_E_SCHEMA;
    }
    err_buf[0] = '\0';
    return SF_OK;
  } catch (const std::exception& e) {
    const std::string msg = std::string("JSON parse error: ") + e.what();
    sfcore::set_last_error(msg);
    std::strncpy(err_buf, msg.c_str(), err_cap - 1);
    err_buf[err_cap - 1] = '\0';
    return SF_E_SCHEMA;
  } catch (...) {
    sfcore::set_last_error("validate: unknown exception");
    return SF_E_SCHEMA;
  }
}
