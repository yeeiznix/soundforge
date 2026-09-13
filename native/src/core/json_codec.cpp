// SoundForge G0 — SfProjectDoc ↔ JSON codec (§3, §9).
// nlohmann::json objects use std::map ordering → keys serialize sorted,
// giving deterministic output for identical documents.
#include "sf_internal.hpp"

namespace sfcore {

json envelope_to_json(const ObjectEnvelope& e) {
  json j = json::object();
  j["id"] = e.id;
  j["type"] = e.type;
  j["version"] = e.version;
  j["createdAt"] = e.createdAt;
  j["modifiedAt"] = e.modifiedAt;
  j["provenance"] = e.provenance;
  j["data"] = e.data;
  return j;
}

bool envelope_from_json(const json& j, ObjectEnvelope& out, std::string& err) {
  if (!j.is_object()) {
    err = "envelope: expected object";
    return false;
  }
  auto get_str = [&](const char* k, const std::string& def) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : def;
  };
  out = ObjectEnvelope{};
  out.id = get_str("id", "");
  out.type = get_str("type", "");
  if (j.contains("version") && j["version"].is_number_integer()) {
    out.version = j["version"].get<int>();
  }
  out.createdAt = get_str("createdAt", "");
  out.modifiedAt = get_str("modifiedAt", "");
  out.provenance = get_str("provenance", "created");
  if (j.contains("data") && j["data"].is_object()) out.data = j["data"];
  return true;
}

json audit_to_json(const AuditEntry& a) {
  json j = json::object();
  j["ts"] = a.ts;
  j["actor"] = a.actor;
  j["action"] = a.action;
  j["objectId"] = a.objectId;
  j["detail"] = a.detail;
  return j;
}

bool audit_from_json(const json& j, AuditEntry& out, std::string& err) {
  if (!j.is_object()) {
    err = "entry: expected object";
    return false;
  }
  auto get_str = [&](const char* k, const std::string& def) {
    return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : def;
  };
  out.ts = get_str("ts", "");
  out.actor = get_str("actor", "");
  out.action = get_str("action", "");
  out.objectId = get_str("objectId", "");
  out.detail = get_str("detail", "");
  return true;
}

json doc_to_json(const SfProjectDoc& d) {
  json j = json::object();
  j["schemaVersion"] = d.schemaVersion;
  j["engineVersion"] = d.engineVersion;

  json pr = json::object();
  pr["id"] = d.project.id;
  pr["name"] = d.project.name;
  pr["createdAt"] = d.project.createdAt;
  pr["modifiedAt"] = d.project.modifiedAt;
  pr["author"] = d.project.author;
  pr["notes"] = d.project.notes;
  j["project"] = pr;

  json v = json::object();
  v["id"] = d.venue.id;
  v["name"] = d.venue.name;
  json dims = json::object();
  dims["widthM"] = d.venue.widthM;
  dims["depthM"] = d.venue.depthM;
  dims["heightM"] = d.venue.heightM;
  v["dimensions"] = dims;
  j["venue"] = v;

  json sc = json::object();
  sc["id"] = d.scene.id;
  sc["name"] = d.scene.name;
  sc["venueRef"] = d.scene.venueRef;
  auto point3 = [](const Point3& pt) {
    json p = json::object();
    p["x"] = pt.x;
    p["y"] = pt.y;
    p["z"] = pt.z;
    return p;
  };
  json geo = json::object();
  geo["center"] = point3(d.scene.center);
  geo["listening"] = point3(d.scene.listening);
  sc["geometry"] = geo;
  j["scene"] = sc;

  auto envelopes = [](const std::vector<ObjectEnvelope>& vec) {
    json arr = json::array();
    for (const auto& e : vec) arr.push_back(envelope_to_json(e));
    return arr;
  };

  j["audienceReceivers"] = envelopes(d.audienceReceivers);
  j["equipment"] = envelopes(d.equipment);
  json signalGraphJson = json::object();
  json nodes = json::array();
  for (const auto& node : d.signalGraph.nodes) {
    json n = json::object();
    n["kind"] = node_kind_name(static_cast<SignalNodeKind>(node.kind));
    n["id"] = node.id;
    n["label"] = node.name;
    n["position"] = {{"x", node.position.x}, {"y", node.position.y}};
    n["mixer"] = {{"mute", node.mixer.mute},
                  {"solo", node.mixer.solo},
                  {"gainDb", node.mixer.gainDb},
                  {"pan", node.mixer.pan}};
    nodes.push_back(n);
  }
  json edges = json::array();
  for (const auto& edge : d.signalGraph.edges) {
    json e = json::object();
    e["from"] = edge.fromNodeId;
    e["to"] = edge.toNodeId;
    e["label"] = edge.label;
    if (!edge.id.empty()) e["id"] = edge.id;  // G2 P7: edge ids cross the wire
    edges.push_back(e);
  }
  signalGraphJson["nodes"] = nodes;
  signalGraphJson["edges"] = edges;
  j["signalGraph"] = signalGraphJson;
  j["powerGraph"] = d.powerGraph;
  j["audioAssets"] = envelopes(d.audioAssets);
  j["dspPresets"] = envelopes(d.dspPresets);
  j["arrayConfigurations"] = envelopes(d.arrayConfigurations);
  j["measurements"] = envelopes(d.measurements);
  j["simulationRuns"] = envelopes(d.simulationRuns);
  j["trainingScenarios"] = envelopes(d.trainingScenarios);
  j["inventoryRefs"] = envelopes(d.inventoryRefs);
  j["reports"] = envelopes(d.reports);

  json audits = json::array();
  for (const auto& a : d.auditLog) audits.push_back(audit_to_json(a));
  j["auditLog"] = audits;

  return j;
}

bool doc_from_json(const json& j, SfProjectDoc& out, std::string& err) {
  if (!j.is_object()) {
    err = "root: expected JSON object";
    return false;
  }
  out = SfProjectDoc{};

  if (j.contains("schemaVersion") && j["schemaVersion"].is_number_integer())
    out.schemaVersion = j["schemaVersion"].get<int>();
  if (j.contains("engineVersion") && j["engineVersion"].is_string())
    out.engineVersion = j["engineVersion"].get<std::string>();

  auto get_str = [](const json& o, const char* k, const std::string& def) {
    return (o.contains(k) && o[k].is_string()) ? o[k].get<std::string>() : def;
  };

  if (!j.contains("project") || !j["project"].is_object()) {
    err = "missing 'project' object";
    return false;
  }
  const json& pr = j["project"];
  out.project.id = get_str(pr, "id", "");
  out.project.name = get_str(pr, "name", "");
  out.project.createdAt = get_str(pr, "createdAt", "");
  out.project.modifiedAt = get_str(pr, "modifiedAt", "");
  out.project.author = get_str(pr, "author", "");
  out.project.notes = get_str(pr, "notes", "");

  auto get_num = [](const json& o, const char* k, double def) {
    return (o.contains(k) && o[k].is_number()) ? o[k].get<double>() : def;
  };
  auto get_point3 = [&get_num](const json& o) {
    Point3 pt;
    pt.x = get_num(o, "x", 0.0);
    pt.y = get_num(o, "y", 0.0);
    pt.z = get_num(o, "z", 0.0);
    return pt;
  };

  if (j.contains("venue") && j["venue"].is_object()) {
    out.venue.id = get_str(j["venue"], "id", "");
    out.venue.name = get_str(j["venue"], "name", "");
    if (j["venue"].contains("dimensions") && j["venue"]["dimensions"].is_object()) {
      const json& dims = j["venue"]["dimensions"];
      out.venue.widthM = get_num(dims, "widthM", 0.0);
      out.venue.depthM = get_num(dims, "depthM", 0.0);
      out.venue.heightM = get_num(dims, "heightM", 0.0);
    }
  }
  if (j.contains("scene") && j["scene"].is_object()) {
    out.scene.id = get_str(j["scene"], "id", "");
    out.scene.name = get_str(j["scene"], "name", "");
    out.scene.venueRef = get_str(j["scene"], "venueRef", "");
    if (j["scene"].contains("geometry") && j["scene"]["geometry"].is_object()) {
      const json& geo = j["scene"]["geometry"];
      if (geo.contains("center") && geo["center"].is_object())
        out.scene.center = get_point3(geo["center"]);
      if (geo.contains("listening") && geo["listening"].is_object())
        out.scene.listening = get_point3(geo["listening"]);
    }
  }

  if (j.contains("signalGraph") && j["signalGraph"].is_object()) {
  const json& sg = j["signalGraph"];
  if (sg.contains("nodes") && sg["nodes"].is_array()) {
    for (const auto& n : sg["nodes"]) {
      SignalNode node;
      node.id = n.value("id", "");
      node.kind = static_cast<int>(parse_node_kind(n.value("kind", "source")));
      node.name = n.value("label", "");
      if (n.contains("position") && n["position"].is_object()) {
        node.position.x = n["position"].value("x", 0.0);
        node.position.y = n["position"].value("y", 0.0);
      }
      if (n.contains("mixer") && n["mixer"].is_object()) {
        node.mixer.mute = n["mixer"].value("mute", false);
        node.mixer.solo = n["mixer"].value("solo", false);
        node.mixer.gainDb = n["mixer"].value("gainDb", 0.0);
        node.mixer.pan = n["mixer"].value("pan", 0.0);
      }
      out.signalGraph.nodes.push_back(node);
    }
  }
  if (sg.contains("edges") && sg["edges"].is_array()) {
    for (const auto& e : sg["edges"]) {
      SignalEdge edge;
      edge.id = e.value("id", "");
      edge.fromNodeId = e.value("from", "");
      edge.toNodeId = e.value("to", "");
      edge.label = e.value("label", "");
      out.signalGraph.edges.push_back(edge);
    }
  }
}
  if (j.contains("powerGraph") && j["powerGraph"].is_object()) out.powerGraph = j["powerGraph"];

  auto read_collection = [&](const char* key, std::vector<ObjectEnvelope>& dst) -> bool {
    dst.clear();
    if (!j.contains(key) || !j[key].is_array()) return true;
    for (const auto& e : j[key]) {
      ObjectEnvelope env;
      std::string eErr;
      if (!envelope_from_json(e, env, eErr)) {
        err = std::string(key) + ": " + eErr;
        return false;
      }
      dst.push_back(std::move(env));
    }
    return true;
  };

  if (!read_collection("audienceReceivers", out.audienceReceivers)) return false;
  if (!read_collection("equipment", out.equipment)) return false;
  if (!read_collection("audioAssets", out.audioAssets)) return false;
  if (!read_collection("dspPresets", out.dspPresets)) return false;
  if (!read_collection("arrayConfigurations", out.arrayConfigurations)) return false;
  if (!read_collection("measurements", out.measurements)) return false;
  if (!read_collection("simulationRuns", out.simulationRuns)) return false;
  if (!read_collection("trainingScenarios", out.trainingScenarios)) return false;
  if (!read_collection("inventoryRefs", out.inventoryRefs)) return false;
  if (!read_collection("reports", out.reports)) return false;

  if (j.contains("auditLog") && j["auditLog"].is_array()) {
    for (const auto& a : j["auditLog"]) {
      AuditEntry ae;
      std::string aErr;
      if (!audit_from_json(a, ae, aErr)) {
        err = "auditLog: " + aErr;
        return false;
      }
      out.auditLog.push_back(std::move(ae));
    }
  }
  return true;
}

int peek_schema_version(const json& j) {
  if (j.is_object() && j.contains("schemaVersion") && j["schemaVersion"].is_number_integer())
    return j["schemaVersion"].get<int>();
  return -1;  // unknown/invalid
}

}  // namespace sfcore
