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
  j["venue"] = v;

  json sc = json::object();
  sc["id"] = d.scene.id;
  sc["name"] = d.scene.name;
  sc["venueRef"] = d.scene.venueRef;
  j["scene"] = sc;

  auto envelopes = [](const std::vector<ObjectEnvelope>& vec) {
    json arr = json::array();
    for (const auto& e : vec) arr.push_back(envelope_to_json(e));
    return arr;
  };

  j["audienceReceivers"] = envelopes(d.audienceReceivers);
  j["equipment"] = envelopes(d.equipment);
  j["signalGraph"] = d.signalGraph;
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

  if (j.contains("venue") && j["venue"].is_object()) {
    out.venue.id = get_str(j["venue"], "id", "");
    out.venue.name = get_str(j["venue"], "name", "");
  }
  if (j.contains("scene") && j["scene"].is_object()) {
    out.scene.id = get_str(j["scene"], "id", "");
    out.scene.name = get_str(j["scene"], "name", "");
    out.scene.venueRef = get_str(j["scene"], "venueRef", "");
  }

  if (j.contains("signalGraph") && j["signalGraph"].is_object()) out.signalGraph = j["signalGraph"];
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
