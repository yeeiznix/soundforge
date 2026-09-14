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
// G4 P1 (PLAN_G4 §4.4 D4): render_chain is now literally "compile a
// preallocated RenderPlan, then execute it". compile_render_plan does the
// topology + active-set + gain/pan/exclusion work ONCE and owns the per-node
// block pool; render_chain_planned is a zero-allocation walk. This fulfils the
// original seam comment ("G4's device-callback drop-in preallocates a pool
// outside the hot path") while preserving the G3 render semantics and float
// operand order EXACTLY, so every G3 test_dsp_render case passes unmodified.
#include "dsp_internal.hpp"
#include "graph_internal.hpp"
#include "render_plan.hpp"

#include <string>

namespace sfcore {

bool render_chain(const SignalGraphDoc& g, dsp::AudioBlock& block,
                  const std::string& out_node_id, std::string& err) {
  dsp::RenderPlan plan;
  if (!compile_render_plan(g, out_node_id, plan, err)) return false;
  return dsp::render_chain_planned(plan, block, err);
}

}  // namespace sfcore
