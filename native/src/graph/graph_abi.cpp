// SoundForge G2 P3 — graph mutators C ABI wrappers (§4.2)
// Compiled as part of sfcore (added to SFCORE_SOURCES), lives in graph/ for cohesion.
#include "sf_internal.hpp"
#include "graph_internal.hpp"
#include "soundforge/sf_graph.h"

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

} // extern "C"