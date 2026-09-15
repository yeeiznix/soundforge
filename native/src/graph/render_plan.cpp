// SoundForge G4 P1 — RenderPlan compile/execute (PLAN_G4 §4.4 D4).
//
// render_chain_planned reproduces the G3 dsp_render.cpp semantics EXACTLY, with
// the same float operand order, but reads a prebuilt plan instead of allocating
// a std::map per call. The G3 map was only ever used for predecessor LOOKUP,
// and a node's predecessors always precede it in topological order, so the
// accumulation order is fixed by the topo walk (edge document order), not by
// the map's key order — which is why the planned output is bit-identical.
#include "render_plan.hpp"

#include "graph_internal.hpp"

#include <algorithm>
#include <cmath>
#include <new>
#include <set>

namespace sfcore::dsp {

bool compile_render_plan(const SignalGraphDoc& g, const std::string& out_node_id,
                         RenderPlan& out_plan, std::string& err) {
  out_plan = RenderPlan{};

  std::vector<std::string> order;
  if (!topological_order(g, order, err)) return false;  // cycle: undefined

  try {
    out_plan.nodes.reserve(order.size());
    out_plan.preds.reserve(order.size());

    // Mute/solo exclusion — identical active-set construction to G3
    // (soloed ∪ transitive downstream ∪ transitive upstream; upstream walk
    // starts from soloed nodes only).
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

    for (const auto& id : order) {
      const SignalNode* nd = find_node(g, id);
      if (!nd) continue;  // unreachable for well-formed order

      PlannedNode pn;
      pn.kind = nd->kind;
      pn.gain = static_cast<float>(std::pow(10.0, nd->mixer.gainDb / 20.0));
      pn.pan = static_cast<float>(nd->mixer.pan);
      pn.pan_active = nd->mixer.pan != 0.0;
      pn.excluded = muted.count(id) > 0 || (solo_mode && active.count(id) == 0);
      out_plan.nodes.push_back(std::move(pn));
    }

    // Predecessor index lists, in edge document order (preserves the G3
    // accumulation order, duplicate edges included).
    out_plan.preds.assign(out_plan.nodes.size(), {});
    for (std::size_t i = 0; i < order.size(); ++i) {
      for (const auto& e : g.edges) {
        if (e.toNodeId != order[i]) continue;
        const auto it = std::find(order.begin(), order.end(), e.fromNodeId);
        if (it == order.end()) continue;  // dangling edge: G3's rendered.find miss
        out_plan.preds[i].push_back(static_cast<int>(it - order.begin()));
      }
    }

    out_plan.node_index.reserve(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      out_plan.node_index.emplace_back(order[i], static_cast<int>(i));
    }
    std::sort(out_plan.node_index.begin(), out_plan.node_index.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const auto oi = std::find(order.begin(), order.end(), out_node_id);
    out_plan.out_index = (oi == order.end()) ? -1 : static_cast<int>(oi - order.begin());
    return true;
  } catch (const std::bad_alloc&) {
    out_plan = RenderPlan{};
    err = "render_plan: out of memory";
    return false;
  }
}

bool render_chain_planned(RenderPlan& plan, AudioBlock& block, std::string& err) {
  // SEC-G4-04 defensive entry bound (ORC-G4P4-02): reject an oversized block
  // before the empty-plan fast-path and before any per-node scratch write. The
  // engine tick already bounds frames; this is mandated defense-in-depth for
  // direct callers of the render seam.
  if (block.n > kBlockMaxSamples) {
    err = "render_plan: block exceeds max";
    return false;
  }

  const std::size_t n = block.n;

  // Empty graph: nothing to process — the block passes through unchanged
  // (exact G3 semantics).
  if (plan.nodes.empty()) return true;

  for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
    PlannedNode& pn = plan.nodes[i];

    // Node scratch is reused across executes; mix_bus ACCUMULATES, so the
    // active window must start zeroed (G3 value-initialized a fresh block for
    // EVERY node, target included).
    for (std::size_t j = 0; j < n; ++j) {
      pn.block.L[j] = 0.0f;
      pn.block.R[j] = 0.0f;
    }
    pn.block.n = n;

    if (pn.kind == SfNodeSource) {
      // The input block is fed to EVERY source node.
      for (std::size_t j = 0; j < n; ++j) {
        pn.block.L[j] = block.L[j];
        pn.block.R[j] = block.R[j];
      }
    } else {
      for (const int pi : plan.preds[i]) {
        const AudioBlock& src = plan.nodes[static_cast<std::size_t>(pi)].block;
        if (src.n != n) continue;  // mirrors G3's src->second.n != n skip
        mix_bus(pn.block.L, src.L, n);
        mix_bus(pn.block.R, src.R, n);
      }
    }

    if (pn.excluded) {
      for (std::size_t j = 0; j < n; ++j) {
        pn.block.L[j] = 0.0f;
        pn.block.R[j] = 0.0f;
      }
    } else {
      apply_gain(pn.block.L, n, pn.gain);
      apply_gain(pn.block.R, n, pn.gain);
      if (pn.pan_active) apply_pan(pn.block.L, pn.block.R, n, pn.pan);
    }
  }

  if (plan.out_index >= 0) {
    const AudioBlock& tgt = plan.nodes[static_cast<std::size_t>(plan.out_index)].block;
    for (std::size_t j = 0; j < n; ++j) {
      block.L[j] = tgt.L[j];
      block.R[j] = tgt.R[j];
    }
    return true;
  }

  // Unknown target inside a non-empty graph: same silence evaluate_mixer would
  // produce (exact G3 message).
  err = "dsp_render: target node not found in graph";
  for (std::size_t j = 0; j < n; ++j) {
    block.L[j] = 0.0f;
    block.R[j] = 0.0f;
  }
  return false;
}

}  // namespace sfcore::dsp
