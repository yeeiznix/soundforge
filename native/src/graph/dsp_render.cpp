// SoundForge G3 P3 — chain-render harness (PLAN_G3 §4.2 D2 / §6 P3, §1.2
// "Chain-render harness" row: native/src/graph/dsp_render.cpp, compiled INTO
// sfgraph). This is LIBRARY code, not test-local: the host-testable proof
// that the mixing law + kernels compose, and the seam G4 connects a real
// device callback to (§4.2 rationale — avoids the "desk estimate diverges
// from the engine" hazard: evaluate_mixer and this harness share the same
// sfcore::dsp law + kernels).
//
// Walks the topological order of a runtime SignalGraphDoc and pushes a
// caller-owned finite block (dsp::AudioBlock, D2 shape) through every
// connected node's sfdsp kernels (apply_gain / apply_pan / apply_gate --
// via mute/solo exclusion / mix_bus). Node semantics mirror evaluate_mixer
// (routing.cpp):
//   - the input block is fed to EVERY source node;
//   - a node's output = gate * gain * pan( SUM over in-edges of the
//     predecessors' outputs ); mute/solo exclusion zeroes the node (silence
//     propagates downstream, matching the path-invalidation semantics);
//   - pan == 0.0 (the default) means "no pan operation" — apply_pan is called
//     only for pan != 0 nodes (dsp_internal.hpp apply_pan: kernel law and
//     harness convention coexist by design).
//
// Kernels are stateless and sample-wise, so block segmentation cannot change
// the output (asserted by the P3 1024-sample split test — §6 P3 acceptance).
//
// Host path only (G3): one AudioBlock buffer per node, std::map-backed, per
// render call. No locks/malloc inside the kernels (sfdsp leaf invariant);
// G4's device-callback drop-in preallocates a pool outside the hot path.
#include "dsp_internal.hpp"
#include "graph_internal.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sfcore {

bool render_chain(const SignalGraphDoc& g, dsp::AudioBlock& block,
                  const std::string& out_node_id, std::string& err) {
  std::vector<std::string> order;
  if (!topological_order(g, order, err)) return false;  // cycle: undefined

  // Mute/solo exclusion — same active-set construction as evaluate_mixer
  // (§4.3: soloed ∪ transitive downstream ∪ transitive upstream; upstream
  // walk starts from soloed nodes only). Kept in sync with evaluate_mixer().
  std::set<std::string> muted, soloed;
  for (const auto& nd : g.nodes) {
    if (nd.mixer.mute) muted.insert(nd.id);
    if (nd.mixer.solo) soloed.insert(nd.id);
  }
  std::set<std::string> active;
  const bool solo_mode = !soloed.empty();
  if (solo_mode) {
    active.insert(soloed.begin(), soloed.end());
    std::vector<std::string> stack(soloed.begin(), soloed.end());
    std::set<std::string> seen = active;
    while (!stack.empty()) {
      const std::string id = stack.back();
      stack.pop_back();
      for (const auto& e : g.edges) {
        if (e.fromNodeId == id && !seen.count(e.toNodeId)) {
          seen.insert(e.toNodeId);
          active.insert(e.toNodeId);
          stack.push_back(e.toNodeId);
        }
      }
    }
    stack.assign(soloed.begin(), soloed.end());
    while (!stack.empty()) {
      const std::string id = stack.back();
      stack.pop_back();
      for (const auto& e : g.edges) {
        if (e.toNodeId == id && !seen.count(e.fromNodeId)) {
          seen.insert(e.fromNodeId);
          active.insert(e.fromNodeId);
          stack.push_back(e.fromNodeId);
        }
      }
    }
  }

  const std::size_t n = block.n;
  std::map<std::string, dsp::AudioBlock> rendered;
  for (const auto& id : order) {
    const SignalNode* nd = find_node(g, id);
    if (!nd) continue;

    // Value-init zeroes the bus (mix_bus ACCUMULATES — must start at 0).
    dsp::AudioBlock b{};
    b.n = n;
    if (nd->kind == SfNodeSource) {
      for (std::size_t i = 0; i < n; ++i) {
        b.L[i] = block.L[i];
        b.R[i] = block.R[i];
      }
    } else {
      // Sum every in-edge's predecessor output onto this node's bus
      // (predecessors precede this node in topo order -> already rendered).
      for (const auto& e : g.edges) {
        if (e.toNodeId != id) continue;
        const auto src = rendered.find(e.fromNodeId);
        if (src == rendered.end() || src->second.n != n) continue;
        dsp::mix_bus(b.L, src->second.L, n);
        dsp::mix_bus(b.R, src->second.R, n);
      }
    }

    const bool excluded = muted.count(id) || (solo_mode && !active.count(id));
    if (excluded) {
      for (std::size_t i = 0; i < n; ++i) {
        b.L[i] = 0.0f;
        b.R[i] = 0.0f;
      }
    } else {
      const float g =
          static_cast<float>(std::pow(10.0, nd->mixer.gainDb / 20.0));
      dsp::apply_gain(b.L, n, g);
      dsp::apply_gain(b.R, n, g);
      if (nd->mixer.pan != 0.0) {
        dsp::apply_pan(b.L, b.R, n, static_cast<float>(nd->mixer.pan));
      }
    }
    rendered[id] = std::move(b);
  }

  const auto it = rendered.find(out_node_id);
  if (it != rendered.end()) {
    // Copy the target node's output back into the caller-owned block.
    for (std::size_t i = 0; i < n; ++i) {
      block.L[i] = it->second.L[i];
      block.R[i] = it->second.R[i];
    }
    return true;
  }
  if (!rendered.empty()) {
    // Unknown target inside a non-empty graph: nothing routes to it -> the
    // same silence an output node with no valid inputs would produce.
    err = "dsp_render: target node not found in graph";
    for (std::size_t i = 0; i < n; ++i) {
      block.L[i] = 0.0f;
      block.R[i] = 0.0f;
    }
    return false;
  }
  // Empty graph: nothing to process — the block passes through unchanged.
  return true;
}

}  // namespace sfcore