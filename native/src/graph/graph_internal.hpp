// Internal graph structures (§3.2) — NOT part of the public ABI.
// Included by sf_internal.hpp BEFORE its `using Uuid`/`using json` aliases,
// so this header must compile standalone with concrete std types.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sfcore {

enum SignalNodeKind : int { SfNodeSource = 1, SfNodeProcessor, SfNodeOutput, SfNodeBus };

struct Point2 {
  double x = 0.0;
  double y = 0.0;
};

struct SfMixerState {
  double gainDb = 0.0;  // [-60, 24]; not on the v2 wire (P3 mutators)
  double pan = 0.0;     // [-1, 1];   not on the v2 wire (P3 mutators)
  bool mute = false;
  bool solo = false;
};

struct SignalNode {
  std::string id;
  int kind = SfNodeSource;
  std::string name;  // wire "label"
  Point2 position;
  SfMixerState mixer;
};

struct SignalEdge {
  std::string id;          // "" for now; not on the v2 wire (P3 adds ids)
  std::string fromNodeId;  // wire "from"
  std::string toNodeId;    // wire "to"
  std::string label;       // wire "label"
  int fromPort = 0;        // not on the v2 wire (P3)
  int toPort = 0;          // not on the v2 wire (P3)
};

struct SignalGraphDoc {
  std::vector<SignalNode> nodes;
  std::vector<SignalEdge> edges;
};

// Pure helpers (graph.cpp) — no side effects, no ABI.
const SignalNode* find_node(const SignalGraphDoc& g, const std::string& id);
SignalNode* find_node_mut(SignalGraphDoc& g, const std::string& id);
SignalNodeKind parse_node_kind(const std::string& s);
std::string node_kind_name(SignalNodeKind k);

}  // namespace sfcore