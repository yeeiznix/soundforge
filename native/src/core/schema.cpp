// SoundForge G0 — structural validation of project JSON (§3.3, §7.1).
// Mirrors native/data/schemas/project_schema.json (Draft 2020-12) structurally:
// required keys, types, UUID v4 format, RFC3339 timestamps, closed key set.
#include "sf_internal.hpp"

#include <functional>
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
  auto check_bool = [&](const json& obj, const char* field, const char* ctx) {
    if (!obj.contains(field)) {
      add(std::string(ctx) + ": missing '" + field + "'");
      return;
    }
    const auto& v = obj[field];
    if (!v.is_boolean()) {
      add(std::string(ctx) + ": '" + field + "' must be a boolean");
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
    // Collect node ids for edge/mixer reference checks.
    std::set<std::string> node_ids;
    if (g.contains("nodes") && g["nodes"].is_array()) {
      int ni = 0;
      for (const auto& n : g["nodes"]) {
        const std::string nctx = std::string(ctx) + ".nodes[" + std::to_string(ni) + "]";
        if (!n.is_object()) {
          add(nctx + ": expected object");
          ++ni;
          continue;
        }
        if (!n.contains("kind")) {
          add(nctx + ": missing 'kind'");
        } else if (!n["kind"].is_string()) {
          add(nctx + ": 'kind' must be a string");
        } else {
          const std::string kind = n["kind"].get<std::string>();
          if (kind != "source" && kind != "processor" && kind != "output" && kind != "bus")
            add(nctx + ": invalid kind '" + kind + "'");
        }
        if (!n.contains("id")) {
          add(nctx + ": missing 'id'");
        } else if (n["id"].is_string()) {
          if (!is_uuid(n["id"].get<std::string>()))
            add(nctx + ": invalid uuid '" + n["id"].get<std::string>() + "'");
          node_ids.insert(n["id"].get<std::string>());
        }
        if (!n.contains("label")) {
          add(nctx + ": missing 'label'");
        } else if (!n["label"].is_string()) {
          add(nctx + ": 'label' must be a string");
        }
        if (!n.contains("position")) {
          add(nctx + ": missing 'position'");
        } else if (!n["position"].is_object()) {
          add(nctx + ".position: expected object");
        } else {
          const json& pos = n["position"];
          for (const char* ax : {"x", "y"}) {
            if (!pos.contains(ax)) {
              add(nctx + ".position: missing '" + ax + "'");
            } else if (!pos[ax].is_number()) {
              add(nctx + ".position: '" + ax + "' must be a number");
            }
          }
        }
        if (!n.contains("mixer")) {
          add(nctx + ": missing 'mixer'");
        } else if (!n["mixer"].is_object()) {
          add(nctx + ".mixer: expected object");
        } else {
          check_bool(n["mixer"], "mute", (nctx + ".mixer").c_str());
          check_bool(n["mixer"], "solo", (nctx + ".mixer").c_str());
          if (n["mixer"].contains("gainDb") && !n["mixer"]["gainDb"].is_number())
            add(nctx + ".mixer: 'gainDb' must be a number");
          if (n["mixer"].contains("pan") && !n["mixer"]["pan"].is_number())
            add(nctx + ".mixer: 'pan' must be a number");
        }
        // SEC-G3-10: dspPresetRef is string|null; missing key tolerated
        // (legacy docs — "" means none). Any other type is rejected so the
        // native validator stays in parity with the Python mirror.
        if (n.contains("dspPresetRef")) {
          if (!n["dspPresetRef"].is_null() && !n["dspPresetRef"].is_string())
            add(nctx + ": 'dspPresetRef' must be a string or null");
        }
        ++ni;
      }
    }
    // Edges: from/to must reference existing nodes; detect cycles.
    std::map<std::string, std::vector<std::string>> adj;
    if (g.contains("edges") && g["edges"].is_array()) {
      int ei = 0;
      for (const auto& e : g["edges"]) {
        const std::string ectx = std::string(ctx) + ".edges[" + std::to_string(ei) + "]";
        if (!e.is_object()) {
          add(ectx + ": expected object");
          ++ei;
          continue;
        }
        bool has_from = false, has_to = false;
        std::string from, to;
        if (!e.contains("from")) {
          add(ectx + ": missing 'from'");
        } else if (e["from"].is_string()) {
          from = e["from"].get<std::string>();
          has_from = true;
          if (!is_uuid(from))
            add(ectx + ": invalid uuid '" + from + "'");
          else if (!node_ids.count(from))
            add(ectx + ": 'from' references unknown node '" + from + "'");
        } else {
          add(ectx + ": 'from' must be a string");
        }
        if (!e.contains("to")) {
          add(ectx + ": missing 'to'");
        } else if (e["to"].is_string()) {
          to = e["to"].get<std::string>();
          has_to = true;
          if (!is_uuid(to))
            add(ectx + ": invalid uuid '" + to + "'");
          else if (!node_ids.count(to))
            add(ectx + ": 'to' references unknown node '" + to + "'");
        } else {
          add(ectx + ": 'to' must be a string");
        }
        if (!e.contains("label")) {
          add(ectx + ": missing 'label'");
        } else if (!e["label"].is_string()) {
          add(ectx + ": 'label' must be a string");
        }
        if (e.contains("id")) {
          if (!e["id"].is_string()) {
            add(ectx + ": 'id' must be a string");
          } else if (!is_uuid(e["id"].get<std::string>())) {
            add(ectx + ": invalid uuid '" + e["id"].get<std::string>() + "'");
          }
        }
        if (has_from && has_to) adj[from].push_back(to);
        ++ei;
      }
      // Directed cycle detection (DFS three-color).
      std::map<std::string, int> color;  // 0=unvisited 1=in-progress 2=done
      std::function<bool(const std::string&)> dfs = [&](const std::string& u) -> bool {
        color[u] = 1;
        for (const auto& v : adj[u]) {
          if (color[v] == 1) return true;       // back edge -> cycle
          if (color[v] == 0 && dfs(v)) return true;
        }
        color[u] = 2;
        return false;
      };
      for (const auto& u : node_ids) {
        if (color[u] == 0 && dfs(u)) {
          add(std::string(ctx) + ": cycle detected in edges");
          break;
        }
      }
    }
    // Mixers: nodeId must reference an existing node.
    if (g.contains("mixers")) {
      if (!g["mixers"].is_array()) {
        add(std::string(ctx) + ".mixers: must be an array");
      } else {
        int mi = 0;
        for (const auto& m : g["mixers"]) {
          const std::string mctx = std::string(ctx) + ".mixers[" + std::to_string(mi) + "]";
          if (!m.is_object()) {
            add(mctx + ": expected object");
            ++mi;
            continue;
          }
          if (!m.contains("nodeId")) {
            add(mctx + ": missing 'nodeId'");
          } else if (m["nodeId"].is_string()) {
            const std::string nid = m["nodeId"].get<std::string>();
            if (!is_uuid(nid))
              add(mctx + ": invalid uuid '" + nid + "'");
            else if (!node_ids.count(nid))
              add(mctx + ": 'nodeId' references unknown node '" + nid + "'");
          } else {
            add(mctx + ": 'nodeId' must be a string");
          }
          if (!m.contains("gains")) {
            add(mctx + ": missing 'gains'");
          } else if (!m["gains"].is_object()) {
            add(mctx + ": 'gains' must be an object");
          } else {
            for (auto git = m["gains"].begin(); git != m["gains"].end(); ++git)
              if (!git.value().is_number())
                add(mctx + ".gains." + git.key() + " must be a number");
          }
          check_bool(m, "mute", mctx.c_str());
          check_bool(m, "solo", mctx.c_str());
          ++mi;
        }
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

// ---------------------------------------------------------------------------
// G3 P7 — JSON depth pre-parse gate (PLAN_G3 §6 P7; SEC-G3-7/-G3-8)
// ---------------------------------------------------------------------------
namespace sfcore {

JsonScanStatus scan_json_depth(const char* data, size_t len, int* max_depth_out) {
  int depth = 0;
  int max_depth = 0;
  bool in_string = false;
  size_t i = 0;
  const size_t n = len;
  auto done = [&](JsonScanStatus st) {
    if (max_depth_out) *max_depth_out = max_depth;
    return st;
  };
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    if (in_string) {
      if (c == '"') {
        in_string = false;
        ++i;
      } else if (c == '\\') {
        // Escape unit = backslash + ONE escaped char. Consuming exactly one
        // extra char is what keeps the state machine honest: an unknown
        // escape (\x) is a 2-char unit and must NOT swallow the following
        // char, and odd/even backslash runs before a quote fall out of this
        // rule naturally (\\ = escaped backslash; \" = escaped quote).
        ++i;  // consume the backslash
        if (i >= n) return done(JsonScanStatus::kUnterminated);  // "\<EOF>
        const unsigned char e = static_cast<unsigned char>(data[i]);
        ++i;
        if (e == 'u') {
          // \uXXXX: consume up to four more chars UNCONDITIONALLY without
          // checking hex-ness. Validity is nlohmann's call, not ours; the
          // point is that <4 or non-hex digits can never desync our string
          // tracking (a '"' swallowed here is part of the escape unit, so it
          // never terminates the string early and braces never leak out).
          for (int k = 0; k < 4 && i < n; ++k) ++i;
        }
        // e == any other char: already consumed — 2-char unit total.
      } else {
        // Any other byte: raw control bytes (0x00-0x1F — nlohmann rejects
        // them as unescaped controls), UTF-8 payload, anything: non-structural.
        ++i;
      }
      continue;
    }
    // Outside a string: only structure characters matter.
    if (c == '"') {
      in_string = true;
      ++i;
    } else if (c == '{' || c == '[') {
      ++depth;
      if (depth > max_depth) max_depth = depth;
      if (depth > kMaxJsonDepth) return done(JsonScanStatus::kDepthExceeded);
      ++i;
    } else if (c == '}' || c == ']') {
      // Stray closer on malformed input: clamp, never trip (a negative depth
      // must not become a false "depth exceeds" verdict — nlohmann judges).
      if (depth > 0) --depth;
      ++i;
    } else {
      ++i;  // whitespace, digits, ':', ',', invalid bytes: non-structural
    }
  }
  if (in_string) return done(JsonScanStatus::kUnterminated);
  return done(JsonScanStatus::kOk);
}

sf_result_t checked_parse(const char* data, size_t len, json* out, std::string* err_out) {
  if (!data) {
    if (err_out) *err_out = "parse: null input";
    return SF_E_INVALID_ARG;
  }
  // (1) 8 MiB byte cap — enforced pre-parse, never post-parse (same code and
  // message from_json used before P7).
  if (len > kMaxDocBytes) {
    if (err_out) *err_out = "JSON input exceeds 8 MiB limit";
    return SF_E_FILE_TOO_LARGE;
  }
  // (2) Depth gate. Distinct pre-reject texts (SEC-G3-8): depth vs.
  // unterminated string are always distinguishable — an unterminated string
  // NEVER trips the depth text even on a >256-deep document.
  int max_depth = 0;
  const JsonScanStatus st = scan_json_depth(data, len, &max_depth);
  if (st == JsonScanStatus::kDepthExceeded) {
    if (err_out) *err_out = "schema: json depth exceeds " + std::to_string(kMaxJsonDepth);
    return SF_E_SCHEMA;
  }
  if (st == JsonScanStatus::kUnterminated) {
    if (err_out) *err_out = "schema: unterminated string in json";
    return SF_E_SCHEMA;
  }
  // (3) nlohmann — untouched parser. Malformed-but-shallow input reaches it
  // and yields the nlohmann verdict; parse exceptions propagate to callers.
  if (out) *out = json::parse(data, data + len);
  return SF_OK;
}

}  // namespace sfcore

extern "C" sf_result_t sf_validate_project_json(const char* json, size_t len, char* err_buf,
                                                size_t err_cap) {
  if (!json || !err_buf || err_cap == 0) {
    sfcore::set_last_error("validate: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    // G3 P7 (SEC-G3-7): shared pre-parse gate — 8 MiB byte cap + depth scan,
    // then nlohmann. Pre-rejects are only byte-cap / depth>256 / unterminated
    // string; everything else reaches nlohmann unchanged (SEC-G3-8).
    std::string perr;
    sfcore::json j;
    const sf_result_t rc = sfcore::checked_parse(json, len, &j, &perr);
    if (rc != SF_OK) {
      sfcore::set_last_error(perr);
      std::strncpy(err_buf, perr.c_str(), err_cap - 1);
      err_buf[err_cap - 1] = '\0';
      return rc;
    }
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
