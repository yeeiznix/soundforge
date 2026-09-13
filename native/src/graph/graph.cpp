#include "graph_internal.hpp"
#include <cstring>
#include <cmath>
#include <set>

namespace sfcore {

const SignalNode* find_node(const SignalGraphDoc& g, const std::string& id) {
  for (const auto& n : g.nodes)
    if (n.id == id) return &n;
  return nullptr;
}

SignalNode* find_node_mut(SignalGraphDoc& g, const std::string& id) {
  for (auto& n : g.nodes)
    if (n.id == id) return &n;
  return nullptr;
}

SignalNodeKind parse_node_kind(const std::string& s) {
  if (s == "source") return SfNodeSource;
  if (s == "processor") return SfNodeProcessor;
  if (s == "output") return SfNodeOutput;
  if (s == "bus") return SfNodeBus;
  return SfNodeSource;  // default
}

std::string node_kind_name(SignalNodeKind k) {
  switch (k) {
    case SfNodeSource: return "source";
    case SfNodeProcessor: return "processor";
    case SfNodeOutput: return "output";
    case SfNodeBus: return "bus";
    default: return "source";
  }
}

sf_result_t add_node_impl(SignalGraphDoc& g, int32_t kind, const char* name,
                          const std::string& id, std::string& err) {
  // Validate kind
  if (kind < SfNodeSource || kind > SfNodeBus) {
    err = "graph.addNode: invalid kind";
    return SF_E_INVALID_ARG;
  }
  // Validate name
  if (!name || *name == '\0') {
    err = "graph.addNode: name must be non-empty";
    return SF_E_INVALID_ARG;
  }
  const size_t name_len = std::strlen(name);
  if (name_len > 64) {
    err = "graph.addNode: name too long (>64 bytes)";
    return SF_E_INVALID_ARG;
  }
  // Validate id format
  if (id.length() != 36) {
    err = "graph.addNode: invalid UUID format";
    return SF_E_INVALID_ARG;
  }
  // Check id uniqueness
  if (find_node(g, id)) {
    err = "graph.addNode: duplicate node ID";
    return SF_E_INVALID_ARG;
  }
  // Create and add node
  SignalNode node;
  node.id = id;
  node.kind = kind;
  node.name = name;
  node.dspPresetRef = "";
  g.nodes.push_back(std::move(node));
  return SF_OK;
}

sf_result_t remove_node_impl(SignalGraphDoc& g, const std::string& node_id,
                             std::string& err, int& edges_removed) {
  SignalNode* node = find_node_mut(g, node_id);
  if (!node) {
    err = "graph.removeNode: node not found";
    return SF_E_NOT_FOUND;
  }
  // Remove incident edges
  edges_removed = 0;
  for (auto it = g.edges.begin(); it != g.edges.end(); ) {
    if (it->fromNodeId == node_id || it->toNodeId == node_id) {
      it = g.edges.erase(it);
      ++edges_removed;
    } else {
      ++it;
    }
  }
  // Remove node
  for (auto it = g.nodes.begin(); it != g.nodes.end(); ++it) {
    if (it->id == node_id) {
      g.nodes.erase(it);
      break;
    }
  }
  return SF_OK;
}

// Helper: DFS to check if to_id is reachable from from_id (cycle detection)
static bool is_reachable(const SignalGraphDoc& g, const std::string& from_id, const std::string& to_id,
                         std::set<std::string>& visited) {
  if (from_id == to_id) return true;
  visited.insert(from_id);
  for (const auto& edge : g.edges) {
    if (edge.fromNodeId == from_id && !visited.count(edge.toNodeId)) {
      if (is_reachable(g, edge.toNodeId, to_id, visited)) return true;
    }
  }
  return false;
}

sf_result_t add_edge_impl(SignalGraphDoc& g, const std::string& id,
                          const std::string& from_id, const std::string& to_id,
                          int32_t from_port, int32_t to_port, std::string& err) {
  // Validate node existence
  if (!find_node(g, from_id)) {
    err = "graph.addEdge: from node not found";
    return SF_E_NOT_FOUND;
  }
  if (!find_node(g, to_id)) {
    err = "graph.addEdge: to node not found";
    return SF_E_NOT_FOUND;
  }
  // No self-loop
  if (from_id == to_id) {
    err = "graph.addEdge: self-loop not allowed";
    return SF_E_INVALID_ARG;
  }
  // No duplicate edge
  for (const auto& edge : g.edges) {
    if (edge.fromNodeId == from_id && edge.toNodeId == to_id &&
        edge.fromPort == from_port && edge.toPort == to_port) {
      err = "graph.addEdge: duplicate edge";
      return SF_E_INVALID_ARG;
    }
  }
  // No edge INTO Source node
  const SignalNode* to_node = find_node(g, to_id);
  if (to_node && to_node->kind == SfNodeSource) {
    err = "graph.addEdge: cannot route into a source node";
    return SF_E_INVALID_ARG;
  }
  // No edge OUT OF Output node
  const SignalNode* from_node = find_node(g, from_id);
  if (from_node && from_node->kind == SfNodeOutput) {
    err = "graph.addEdge: cannot route out of an output node";
    return SF_E_INVALID_ARG;
  }
  // Cycle detection
  std::set<std::string> visited;
  if (is_reachable(g, to_id, from_id, visited)) {
    err = "graph.addEdge: would create cycle";
    return SF_E_INVALID_ARG;
  }
  // Create edge
  SignalEdge edge;
  edge.id = id;
  edge.fromNodeId = from_id;
  edge.toNodeId = to_id;
  edge.fromPort = from_port;
  edge.toPort = to_port;
  g.edges.push_back(std::move(edge));
  return SF_OK;
}

sf_result_t remove_edge_impl(SignalGraphDoc& g, const std::string& edge_id,
                             std::string& err) {
  for (auto it = g.edges.begin(); it != g.edges.end(); ++it) {
    if (it->id == edge_id) {
      g.edges.erase(it);
      return SF_OK;
    }
  }
  err = "graph.removeEdge: edge not found";
  return SF_E_NOT_FOUND;
}

sf_result_t set_mixer_impl(SignalGraphDoc& g, const std::string& node_id,
                           double gain_db, double pan, bool mute, bool solo,
                           std::string& err) {
  SignalNode* node = find_node_mut(g, node_id);
  if (!node) {
    err = "graph.setMixer: node not found";
    return SF_E_NOT_FOUND;
  }
  // Validate gain_db
  if (!std::isfinite(gain_db) || gain_db < -60.0 || gain_db > 24.0) {
    err = "graph.setMixer: invalid gain_db";
    return SF_E_INVALID_ARG;
  }
  // Validate pan
  if (!std::isfinite(pan) || pan < -1.0 || pan > 1.0) {
    err = "graph.setMixer: invalid pan";
    return SF_E_INVALID_ARG;
  }
  node->mixer.gainDb = gain_db;
  node->mixer.pan = pan;
  node->mixer.mute = mute;
  node->mixer.solo = solo;
  return SF_OK;
}

sf_result_t set_preset_impl(SignalGraphDoc& g, const std::string& node_id,
                            const std::string& preset_id, std::string& err) {
  SignalNode* node = find_node_mut(g, node_id);
  if (!node) {
    err = "graph.setPreset: node not found";
    return SF_E_NOT_FOUND;
  }
  node->dspPresetRef = preset_id;
  return SF_OK;
}

}  // namespace sfcore