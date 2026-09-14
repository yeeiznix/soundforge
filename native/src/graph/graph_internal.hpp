// Internal graph structures (§3.2) — NOT part of the public ABI.
// Included by sf_internal.hpp BEFORE its `using Uuid`/`using json` aliases,
// so this header must compile standalone with concrete std types.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "soundforge/sf_types.h"

namespace sfcore {

// Forward decl of the D2 finite block (full type lives in dsp_internal.hpp;
// this header must stay includable from sf_internal.hpp without pulling in
// the sfdsp internals — declarations only need a reference).
namespace dsp {
struct AudioBlock;
}

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
  std::string dspPresetRef;  // "" = none; references dspPresets[].id
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

// Pure mutation helpers (implemented in graph.cpp). Return SF_OK or SF_E_*;
// err is populated on failure with a caller-prefixed message like
// "graph.addNode: name too long".
sf_result_t add_node_impl(SignalGraphDoc& g, int32_t kind, const char* name,
                          const std::string& id, std::string& err);
sf_result_t remove_node_impl(SignalGraphDoc& g, const std::string& node_id,
                             std::string& err, int& edges_removed);
sf_result_t add_edge_impl(SignalGraphDoc& g, const std::string& id,
                          const std::string& from_id, const std::string& to_id,
                          int32_t from_port, int32_t to_port, std::string& err);
sf_result_t remove_edge_impl(SignalGraphDoc& g, const std::string& edge_id,
                             std::string& err);
sf_result_t set_mixer_impl(SignalGraphDoc& g, const std::string& node_id,
                           double gain_db, double pan, bool mute, bool solo,
                           std::string& err);
sf_result_t set_preset_impl(SignalGraphDoc& g, const std::string& node_id,
                            const std::string& preset_id, std::string& err);

// Routing helpers (routing.cpp) — pure graph queries. DAG-safe; the
// topological_order return must be consulted before relying on paths.
bool can_reach(const SignalGraphDoc& g, const std::string& from, const std::string& to);
bool topological_order(const SignalGraphDoc& g, std::vector<std::string>& out_order,
                       std::string& err);
nlohmann::json evaluate_mixer(const SignalGraphDoc& g);

// Chain-render harness (dsp_render.cpp, compiled INTO sfgraph — PLAN_G3 §4.2
// D2 / §6 P3 "Chain-render harness" row): walks the topological order and
// renders a CALLER-OWNED finite block through every connected node's sfdsp
// kernels. `block` is both input (its L/R/n hold the source material) and
// output (on success it holds the rendered target-node block, same n).
// Semantics mirror evaluate_mixer (mute/solo exclusion, pan==0 passthrough —
// see dsp_internal.hpp apply_pan). Empty graph -> identity passthrough.
// Unknown target id inside a non-empty graph -> silence (returns false with
// err set); cycle -> false with err set. No allocation in the kernels; the
// harness buffers are host-side only (G4 preallocates its pool).
// G4 P1 (PLAN_G4 §4.4): re-expressed as compile_render_plan + the
// zero-allocation render_chain_planned (see render_plan.hpp). Signature and
// behavior unchanged.
bool render_chain(const SignalGraphDoc& g, dsp::AudioBlock& block,
                  const std::string& out_node_id, std::string& err);

}  // namespace sfcore