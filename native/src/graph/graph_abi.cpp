// SoundForge G2 P3 — graph mutators C ABI wrappers (§4.2)
// Compiled as part of sfcore (added to SFCORE_SOURCES), lives in graph/ for cohesion.
#include "sf_internal.hpp"
#include "graph_internal.hpp"
#include "soundforge/sf_graph.h"

#include <cstdlib>
#include <cstring>
#include <new>

using namespace sfcore;

extern "C" {

sf_result_t sf_graph_add_node(sf_project_t* p, int32_t kind, const char* name, char* out_id) {
  if (!p || !name || !*name || !out_id) {
    sfcore::set_handle_error(nullptr, "graph.addNode: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    // P4b single-owner guard: FIRST, before any doc-touching work (SEC-G3-4).
    if (runner_busy_mutator(proj, "graph")) return SF_E_IO;
    std::string id = sfcore::uuid_generate();
    std::string err;
    sf_result_t rc = add_node_impl(proj->doc.signalGraph, kind, name, id, err);
    if (rc != SF_OK) {
      sfcore::set_handle_error(proj, err);
      return rc;
    }
    std::strncpy(out_id, id.c_str(), 36);
    out_id[36] = '\0';
    user_audit(proj, "graph.addNode", id, "kind=" + std::to_string(kind) + " name=" + std::string(name));
    return SF_OK;
  } SF_CATCH_ERRORS()
}

sf_result_t sf_graph_remove_node(sf_project_t* p, const char* node_id) {
  if (!p || !node_id || !*node_id) {
    sfcore::set_handle_error(nullptr, "graph.removeNode: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "graph")) return SF_E_IO;  // P4b single-owner
    std::string err;
    int edges_removed = 0;
    sf_result_t rc = remove_node_impl(proj->doc.signalGraph, node_id, err, edges_removed);
    if (rc != SF_OK) {
      sfcore::set_handle_error(proj, err);
      return rc;
    }
    user_audit(proj, "graph.removeNode", node_id,
               "cascaded=" + std::to_string(edges_removed) + " edges");
    return SF_OK;
  } SF_CATCH_ERRORS()
}

sf_result_t sf_graph_add_edge(sf_project_t* p, const char* from_id, const char* to_id,
                              int32_t from_port, int32_t to_port, char* out_id) {
  if (!p || !from_id || !*from_id || !to_id || !*to_id || !out_id) {
    sfcore::set_handle_error(nullptr, "graph.addEdge: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "graph")) return SF_E_IO;  // P4b single-owner
    if (from_port < 0 || to_port < 0) {
      sfcore::set_handle_error(proj, "graph.addEdge: port must be >= 0");
      return SF_E_INVALID_ARG;
    }
    std::string id = sfcore::uuid_generate();
    std::string err;
    sf_result_t rc = add_edge_impl(proj->doc.signalGraph, id, from_id, to_id, from_port, to_port, err);
    if (rc != SF_OK) {
      sfcore::set_handle_error(proj, err);
      return rc;
    }
    std::strncpy(out_id, id.c_str(), 36);
    out_id[36] = '\0';
    user_audit(proj, "graph.addEdge", id, "from=" + std::string(from_id) + " to=" + std::string(to_id));
    return SF_OK;
  } SF_CATCH_ERRORS()
}

sf_result_t sf_graph_remove_edge(sf_project_t* p, const char* edge_id) {
  if (!p || !edge_id || !*edge_id) {
    sfcore::set_handle_error(nullptr, "graph.removeEdge: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "graph")) return SF_E_IO;  // P4b single-owner
    std::string err;
    sf_result_t rc = remove_edge_impl(proj->doc.signalGraph, edge_id, err);
    if (rc != SF_OK) {
      sfcore::set_handle_error(proj, err);
      return rc;
    }
    user_audit(proj, "graph.removeEdge", edge_id, "");
    return SF_OK;
  } SF_CATCH_ERRORS()
}

sf_result_t sf_graph_set_mixer(sf_project_t* p, const char* node_id,
                               double gain_db, double pan, int32_t mute, int32_t solo) {
  if (!p || !node_id || !*node_id) {
    sfcore::set_handle_error(nullptr, "graph.setMixer: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "graph")) return SF_E_IO;  // P4b single-owner
    std::string err;
    sf_result_t rc = set_mixer_impl(proj->doc.signalGraph, node_id, gain_db, pan,
                                    mute != 0, solo != 0, err);
    if (rc != SF_OK) {
      sfcore::set_handle_error(proj, err);
      return rc;
    }
    user_audit(proj, "graph.setMixer", node_id,
               "gain=" + std::to_string(gain_db) + " pan=" + std::to_string(pan) +
               " mute=" + std::to_string(mute) + " solo=" + std::to_string(solo));
    return SF_OK;
  } SF_CATCH_ERRORS()
}

sf_result_t sf_graph_set_preset(sf_project_t* p, const char* node_id, const char* preset_id) {
  if (!p || !node_id || !*node_id) {
    sfcore::set_handle_error(nullptr, "graph.setPreset: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_mutator(proj, "graph")) return SF_E_IO;  // P4b single-owner
    std::string preset = (preset_id && *preset_id) ? std::string(preset_id) : "";
    // Validate preset exists in dspPresets if non-empty
    if (!preset.empty()) {
      bool found = false;
      for (const auto& env : proj->doc.dspPresets) {
        if (env.id == preset) { found = true; break; }
      }
      if (!found) {
        sfcore::set_handle_error(proj, "graph.setPreset: preset not found");
        return SF_E_NOT_FOUND;
      }
    }
    std::string err;
    sf_result_t rc = set_preset_impl(proj->doc.signalGraph, node_id, preset, err);
    if (rc != SF_OK) {
      sfcore::set_handle_error(proj, err);
      return rc;
    }
    user_audit(proj, "graph.setPreset", node_id, "preset=" + preset);
    return SF_OK;
  } SF_CATCH_ERRORS()
}

// ---------------------------------------------------------------------------
// Routing queries (G2 P4, §4.3)
// ---------------------------------------------------------------------------

// Shared by the two malloc'd-JSON exports: serialize and hand over a buffer.
static sf_result_t emit_json(sfcore::SfProject* proj, const sfcore::json& j,
                             const char* tag, char** out_json, size_t* out_len) {
  const std::string s = j.dump(2);
  char* buf = static_cast<char*>(std::malloc(s.size() + 1));
  if (!buf) {
    sfcore::set_handle_error(proj, std::string(tag) + ": oom");
    return SF_E_NOMEM;
  }
  std::memcpy(buf, s.c_str(), s.size() + 1);
  *out_json = buf;
  *out_len = s.size();
  return SF_OK;
}

sf_result_t sf_graph_validate(sf_project_t* p, char* report_buf, size_t report_cap) {
  if (!p || !report_buf || report_cap == 0) {
    sfcore::set_handle_error(nullptr, "graph.validate: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    // P4b read guard (R-B(c)): reject, not contract — never read doc while the
    // runner owns the mutation thread.
    if (runner_busy_reader(proj, "graph")) return SF_E_IO;
    const auto& g = proj->doc.signalGraph;
    sfcore::json report = sfcore::json::object();
    std::vector<std::string> errors, warnings;

    // 1. Duplicate node / edge ids (edge ids may be "" in pre-P3 docs — skip).
    std::set<std::string> seen;
    for (const auto& n : g.nodes) {
      if (!seen.insert(n.id).second) errors.push_back("duplicate node id " + n.id);
    }
    seen.clear();
    for (const auto& e : g.edges) {
      if (!e.id.empty() && !seen.insert(e.id).second) {
        errors.push_back("duplicate edge id " + e.id);
      }
    }

    // 2. Dangling edges.
    std::set<std::string> node_ids;
    for (const auto& n : g.nodes) node_ids.insert(n.id);
    for (const auto& e : g.edges) {
      if (!node_ids.count(e.fromNodeId)) {
        errors.push_back("edge " + e.id + " dangling fromNodeId " + e.fromNodeId);
      }
      if (!node_ids.count(e.toNodeId)) {
        errors.push_back("edge " + e.id + " dangling toNodeId " + e.toNodeId);
      }
    }

    // 3. dspPresetRef must resolve to an existing dspPresets[].id (SEC-G3-9,
    // D6-amd). ""/null = none and never trips the check — wire-level clear
    // semantics equals sf_graph_set_preset(p, n, "").
    std::set<std::string> preset_ids;
    for (const auto& env : proj->doc.dspPresets) preset_ids.insert(env.id);
    for (const auto& n : g.nodes) {
      if (!n.dspPresetRef.empty() && !preset_ids.count(n.dspPresetRef)) {
        errors.push_back("node " + n.id + " dangling dspPresetRef " + n.dspPresetRef);
      }
    }

    // 4. Routing-rule violations: edge into a source, edge out of an output.
    for (const auto& e : g.edges) {
      const auto* to_node = sfcore::find_node(g, e.toNodeId);
      if (to_node && to_node->kind == SfNodeSource) {
        errors.push_back("edge " + e.id + " routes into source node");
      }
      const auto* from_node = sfcore::find_node(g, e.fromNodeId);
      if (from_node && from_node->kind == SfNodeOutput) {
        errors.push_back("edge " + e.id + " routes out of output node");
      }
    }

    // 5. Cycles.
    std::vector<std::string> order;
    std::string topo_err;
    if (!sfcore::topological_order(g, order, topo_err)) {
      errors.push_back("cycle detected: " + topo_err);
    }

    // 6. Orphans — non-source nodes unreachable from any source (warning, §3.3).
    for (const auto& n : g.nodes) {
      if (n.kind == SfNodeSource) continue;
      bool reachable = false;
      for (const auto& s : g.nodes) {
        if (s.kind == SfNodeSource && s.id != n.id && sfcore::can_reach(g, s.id, n.id)) {
          reachable = true;
          break;
        }
      }
      if (!reachable) warnings.push_back("node " + n.id + " not reachable from any source");
    }

    report["status"] = errors.empty() ? (warnings.empty() ? "ok" : "warning") : "error";
    report["warnings"] = warnings;
    report["errors"] = errors;
    sfcore::json stats = sfcore::json::object();
    stats["nodes"] = g.nodes.size();
    stats["edges"] = g.edges.size();
    report["stats"] = stats;

    const std::string s = report.dump(2);
    if (s.size() + 1 > report_cap) {
      sfcore::set_handle_error(proj, "graph.validate: report buffer too small");
      return SF_E_NOMEM;
    }
    std::memcpy(report_buf, s.c_str(), s.size() + 1);
    // Convention shared with sf_project_health_check (G1): SF_OK only when
    // the report is fully clean; warnings AND errors map to SF_E_SCHEMA.
    return report["status"] == "ok" ? SF_OK : SF_E_SCHEMA;
  } SF_CATCH_ERRORS()
}

sf_result_t sf_graph_topological_order(sf_project_t* p, char** out_json, size_t* out_len) {
  if (!p || !out_json || !out_len) {
    sfcore::set_handle_error(nullptr, "graph.topologicalOrder: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_reader(proj, "graph")) return SF_E_IO;  // P4b read guard
    std::vector<std::string> order;
    std::string err;
    if (!sfcore::topological_order(proj->doc.signalGraph, order, err)) {
      sfcore::set_handle_error(proj, "graph.topologicalOrder: " + err);
      return SF_E_SCHEMA;
    }
    sfcore::json j = sfcore::json::object();
    j["order"] = order;
    return emit_json(proj, j, "graph.topologicalOrder", out_json, out_len);
  } SF_CATCH_ERRORS()
}

sf_result_t sf_graph_evaluate_mixer(sf_project_t* p, char** out_json, size_t* out_len) {
  if (!p || !out_json || !out_len) {
    sfcore::set_handle_error(nullptr, "graph.evaluateMixer: null argument");
    return SF_E_INVALID_ARG;
  }
  try {
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    if (runner_busy_reader(proj, "graph")) return SF_E_IO;  // P4b read guard
    // Cycle guard must run before evaluate_mixer trusts the DAG.
    std::vector<std::string> order;
    std::string topo_err;
    if (!sfcore::topological_order(proj->doc.signalGraph, order, topo_err)) {
      sfcore::set_handle_error(proj, "graph.evaluateMixer: " + topo_err);
      return SF_E_SCHEMA;
    }
    return emit_json(proj, sfcore::evaluate_mixer(proj->doc.signalGraph),
                     "graph.evaluateMixer", out_json, out_len);
  } SF_CATCH_ERRORS()
}

} // extern "C"