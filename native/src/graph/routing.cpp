// SoundForge G2 P4 / G3 P3 — routing engine (§4.1: reachability, Kahn topo
// sort, static mixer evaluation). Pure graph math on SignalGraphDoc: no
// SfProject dependency, no audio buffers, no sample values (G3).
//
// Boundary discipline (§4.3): evaluate_mixer() computes *routing-desk*
// estimates — topological order and per-route static scalar coefficients in
// the linear amplitude domain. The per-output aggregation is the G3 mixing
// law (sfcore::dsp, PLAN_G3 §4.1 D1): peak/power sums, headroom, clip gate.
// `clipped:true` is a desk indicator only; NO clamping is applied here
// (that lives in the sfdsp kernels / render path, G3).
#include "dsp_internal.hpp"
#include "graph_internal.hpp"

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sfcore {

// ---------------------------------------------------------------------------
// Reachability — shared cycle-detection helper (graph.cpp add_edge_impl and
// the graph validate / health-check paths all route through this one DFS).
// ---------------------------------------------------------------------------
static bool dfs_reach(const SignalGraphDoc& g, const std::string& cur,
                      const std::string& to, std::set<std::string>& visited) {
  if (cur == to) return true;
  visited.insert(cur);
  for (const auto& e : g.edges) {
    if (e.fromNodeId == cur && !visited.count(e.toNodeId)) {
      if (dfs_reach(g, e.toNodeId, to, visited)) return true;
    }
  }
  return false;
}

bool can_reach(const SignalGraphDoc& g, const std::string& from,
               const std::string& to) {
  std::set<std::string> visited;
  return dfs_reach(g, from, to, visited);
}

// ---------------------------------------------------------------------------
// Kahn topological sort — sources first, deterministic (source kind gets
// priority 0, everything else 1; node id breaks ties). A self-loop counts as
// an in-edge that never drains, so it reports as a cycle.
//
// Dangling edges (either endpoint outside the node set) are ignored here —
// validate() and sf_project_health_check() report them as separate errors.
// ---------------------------------------------------------------------------
bool topological_order(const SignalGraphDoc& g, std::vector<std::string>& out_order,
                       std::string& err) {
  out_order.clear();
  std::map<std::string, int> indeg;
  std::map<std::string, int> kind;
  for (const auto& n : g.nodes) {
    indeg[n.id] = 0;
    kind[n.id] = n.kind;
  }
  for (const auto& e : g.edges) {
    if (indeg.count(e.fromNodeId) && indeg.count(e.toNodeId)) {
      indeg[e.toNodeId] += 1;
    }
  }
  // (priority, id) sorted set of ready nodes — deterministic pop order.
  std::map<std::pair<int, std::string>, bool> pending;
  for (const auto& kv : indeg) {
    if (kv.second == 0) {
      const int prio = (kind[kv.first] == SfNodeSource) ? 0 : 1;
      pending[{prio, kv.first}] = true;
    }
  }
  while (!pending.empty()) {
    const std::string id = pending.begin()->first.second;
    pending.erase(pending.begin());
    out_order.push_back(id);
    for (const auto& e : g.edges) {
      if (e.fromNodeId == id && indeg.count(e.toNodeId)) {
        indeg[e.toNodeId] -= 1;
        if (indeg[e.toNodeId] == 0) {
          const int prio = (kind[e.toNodeId] == SfNodeSource) ? 0 : 1;
          pending[{prio, e.toNodeId}] = true;
        }
      }
    }
  }
  if (out_order.size() < g.nodes.size()) {
    err = "routing: cycle detected";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Static routing-desk evaluation (§4.3). JSON shape (alphabetical keys per
// level, preserved by nlohmann dump):
//
//   top:        { muted, order, outputs, soloed }
//   per output: { clipped, headroomDb, nodeId, peakGainLin, powerGainLin,
//                 routes }
//   per route:  { gainLin, nodeIds, sourceId }
//
// Semantics:
//   - gainLin per (source, output) = SUM over all distinct paths of the
//     path product of 10^(gainDb/20) (each node on the path, incl. source
//     and output, contributes its own mixer gain — a desk pre/post estimate,
//     PLAN_G2 §4.3 boundary note).
//   - A path is valid only when EVERY node on it is not muted and (in solo
//     mode) is in the active set.
//   - Solo mode: active = soloed ∪ transitive downstream ∪ transitive
//     upstream (upstream walk starts from soloed nodes only, so sibling
//     branches feeding the same output stay silent).
//   - Output aggregation = mixing law (sfcore::dsp, G3 D1): peakGainLin =
//     coherent worst case Σ|g| over ALL routed sources (0.0 when no routes).
//     Single-source graphs are byte-compatible with the G2 max (oracle R-A:
//     one route -> Σ ≡ max, identical double). powerGainLin = √Σg² brackets
//     the uncorrelated level (max ≤ power ≤ peak for gains ≥ 0); headroomDb
//     = -20·log10(peak), null on empty routes (peak ≤ 0 -> log10 undefined);
//     clipped <==> headroomDb < 0 (== the G2 predicate peak > 1.0 — pure
//     refactor, log10 strictly increasing). Desk indicators only — no
//     clamping here (G3 kernels).
//   - On a cycle topological_order fails; we return {"error": ...} and the
//     ABI layer maps that to SF_E_SCHEMA.
// ---------------------------------------------------------------------------
nlohmann::json evaluate_mixer(const SignalGraphDoc& g) {
  using json = nlohmann::json;
  json result = json::object();

  std::vector<std::string> order;
  std::string topo_err;
  if (!topological_order(g, order, topo_err)) {
    result["error"] = topo_err;
    return result;
  }

  std::set<std::string> muted, soloed;
  for (const auto& n : g.nodes) {
    if (n.mixer.mute) muted.insert(n.id);
    if (n.mixer.solo) soloed.insert(n.id);
  }

  std::set<std::string> active;
  const bool solo_mode = !soloed.empty();
  if (solo_mode) {
    active.insert(soloed.begin(), soloed.end());
    // Transitive downstream from every soloed node.
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
    // Transitive upstream toward every soloed node (roots stay soloed):
    // in a DAG no node is both downstream and upstream of the same root,
    // so reusing `seen` across both walks cannot hide a contributor.
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

  // Reverse adjacency: toNodeId -> incident edges (drives backward walks).
  std::map<std::string, std::vector<const SignalEdge*>> rev;
  for (const auto& e : g.edges) rev[e.toNodeId].push_back(&e);

  json outputs = json::array();
  for (const auto& out : g.nodes) {
    if (out.kind != SfNodeOutput) continue;

    // sourceId -> summed linear gain over all valid source->output paths;
    // sourceId -> shortest valid path node ids (display path).
    std::map<std::string, double> route_gains;
    std::map<std::string, std::vector<std::string>> route_paths;

    for (const auto& src : g.nodes) {
      if (src.kind != SfNodeSource) continue;
      if (!can_reach(g, src.id, out.id)) continue;

      // Enumerate all distinct paths source -> output (edges in insertion
      // order; deterministic). DAG -> every walk terminates.
      std::vector<std::vector<std::string>> paths;
      std::vector<std::string> cur;
      std::set<std::string> on_stack;
      std::function<void(const std::string&)> walk = [&](const std::string& id) {
        cur.push_back(id);
        on_stack.insert(id);
        if (id == src.id) {
          paths.push_back(std::vector<std::string>(cur.rbegin(), cur.rend()));
        } else {
          for (const SignalEdge* e : rev[id]) {
            if (!on_stack.count(e->fromNodeId)) walk(e->fromNodeId);
          }
        }
        on_stack.erase(id);
        cur.pop_back();
      };
      walk(out.id);

      double sum = 0.0;
      std::vector<std::string> best;
      for (const auto& path : paths) {
        bool ok = true;
        double gain = 1.0;
        for (const auto& nid : path) {
          const SignalNode* n = find_node(g, nid);
          if (!n) {
            ok = false;
            break;
          }
          if (muted.count(nid)) {
            ok = false;
            break;
          }
          if (solo_mode && !active.count(nid)) {
            ok = false;
            break;
          }
          gain *= std::pow(10.0, n->mixer.gainDb / 20.0);
        }
        if (!ok) continue;
        sum += gain;
        if (best.empty() || path.size() < best.size()) best = path;
      }
      if (!best.empty()) {
        route_gains[src.id] = sum;
        route_paths[src.id] = best;
      }
    }

    // Mixing-law aggregation (G3 P3, D1): ONE merge_law call per output —
    // routing.cpp's only law call site (§4.1 / §6 P3: `routing.cpp (via
    // sfcore::dsp::merge_law)`). peakGainLin = coherent worst case Σ|g| over
    // ALL routed sources — identical to the G2 max for the single-source
    // case (oracle R-A), so single-source serialized values are
    // byte-compatible. Route gains are strictly positive (10^(gainDb/20)),
    // so Σ|g| ≡ Σg here.
    std::vector<double> gains;
    gains.reserve(route_gains.size());
    for (const auto& kv : route_gains) gains.push_back(kv.second);

    json routes = json::array();
    for (const auto& kv : route_gains) {
      json r = json::object();
      r["gainLin"] = kv.second;
      r["nodeIds"] = route_paths[kv.first];
      r["sourceId"] = kv.first;
      routes.push_back(r);
    }

    const dsp::MergeLaw law = dsp::merge_law(gains);

    // Insertion order == key order for dump() — alphabetical per level (§4.3):
    // clipped, headroomDb, nodeId, peakGainLin, powerGainLin, routes.
    json o = json::object();
    o["clipped"] = law.clipped;
    if (law.headroom.valid) {
      o["headroomDb"] = law.headroom.db;
    } else {
      o["headroomDb"] = json();  // null — empty routes (D1 verdict note)
    }
    o["nodeId"] = out.id;
    o["peakGainLin"] = law.peak;
    o["powerGainLin"] = law.power;
    o["routes"] = routes;
    outputs.push_back(o);
  }

  json muted_arr = json::array();
  for (const auto& id : muted) muted_arr.push_back(id);
  json soloed_arr = json::array();
  for (const auto& id : soloed) soloed_arr.push_back(id);

  // Insertion order == key order for dump() — alphabetical per level (§4.3).
  result["muted"] = muted_arr;
  result["order"] = order;
  result["outputs"] = outputs;
  result["soloed"] = soloed_arr;
  return result;
}

}  // namespace sfcore