#include "graph_internal.hpp"

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

}  // namespace sfcore