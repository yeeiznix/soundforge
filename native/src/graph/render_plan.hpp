// SoundForge G4 P1 — RenderPlan: the preallocation seam promised by
// dsp_render.cpp:27 ("G4's device-callback drop-in preallocates a pool outside
// the hot path"). PLAN_G4 §4.4 D4.
//
// Lives in sfgraph (NOT sfdsp): compile_render_plan consumes SignalGraphDoc and
// calls topological_order / find_node (graph_internal.hpp). Putting it in sfdsp
// would invert the acyclic sfgraph -> sfdsp edge into a cycle (§2 link
// discipline, review R-B). sfdsp stays a zero-dependency leaf.
//
// Division of labour:
//   * compile_render_plan — ALLOCATES; runs off the hot path (runner thread at
//     snapshot-publish time, or the one-shot G3 harness wrapper). Resolves
//     topology, the mute/solo active set, and per-node gain/pan/exclusion once.
//   * render_chain_planned — ZERO allocation on the success path. Walks the
//     prebuilt node vector, accumulating predecessors by prebuilt index lists.
//
// The plan owns one AudioBlock per node (N x ~4 KB): that pool IS the scratch
// the render writes into, which is what makes the hot path allocation-free.
// A plan is single-reader (the engine holds the snapshot slot exclusively
// while READING), so mutating the owned scratch during execute is safe.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "dsp_internal.hpp"  // dsp::AudioBlock, kBlockMaxSamples

namespace sfcore {

struct SignalGraphDoc;  // graph_internal.hpp

namespace dsp {

// One graph node, precompiled. `block` is the preallocated pool + scratch.
struct PlannedNode {
  AudioBlock block;        // L/R sized kBlockMaxSamples; n set per execute
  int kind = 0;            // SfNodeSource | Processor | Output | Bus
  float gain = 1.0f;       // 10^(gainDb/20), precomputed (G3 expression)
  float pan = 0.0f;        // static_cast<float>(mixer.pan)
  bool pan_active = false; // pan != 0 -> apply_pan (G3 harness convention)
  bool excluded = false;   // mute/solo exclusion, precomputed
};

struct RenderPlan {
  std::vector<PlannedNode> nodes;              // topo order == index order
  std::vector<std::vector<int>> preds;         // preds[i] = predecessor indices
                                               // in EDGE DOCUMENT ORDER (G3
                                               // accumulation order is preserved)
  int out_index = -1;                          // target index, -1 if not found
  // id -> topo index, sorted by id. Diagnostic/off-hot-path only (execute uses
  // indices exclusively).
  std::vector<std::pair<std::string, int>> node_index;
};

// Compile at publish time. ALLOCATES. Returns false (err set) on a cycle
// (delegated to topological_order) or an allocation failure. An unknown
// out_node_id is NOT a compile failure: the plan is valid with out_index == -1
// and render_chain_planned reproduces G3's "silence + false" execute semantics.
bool compile_render_plan(const SignalGraphDoc& g, const std::string& out_node_id,
                         RenderPlan& out_plan, std::string& err);

// Execute with zero allocation on the success path. `plan` is non-const because
// its per-node blocks are the scratch pool. `block` is input (source material)
// and, on success, output (the target node's rendered block), same n.
bool render_chain_planned(RenderPlan& plan, AudioBlock& block, std::string& err);

}  // namespace dsp
}  // namespace sfcore
