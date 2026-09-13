// SoundForge G2 P4 — mixer routing evaluation C ABI (PLAN_G2 §4.3).
// Scalar routing-desk math only: per-route static gain coefficients, mute /
// solo semantics, topo order, clip indicator. No audio buffers anywhere.
#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_graph.h"

#include <array>
#include <cmath>
#include <string>

namespace {

// Run sf_graph_evaluate_mixer, assert SF_OK, return the parsed report.
sfcore::json eval_ok(sf_project_t* p) {
    char* out = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_graph_evaluate_mixer(p, &out, &len), SF_OK);
    std::string s = out ? std::string(out, len) : "";
    if (out) sf_free_string(out);
    return sfcore::json::parse(s);
}

// Evaluate and return the raw dumped report (for key-order assertions).
std::string eval_ok_str(sf_project_t* p) {
    char* out = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_graph_evaluate_mixer(p, &out, &len), SF_OK);
    std::string s = out ? std::string(out, len) : "";
    if (out) sf_free_string(out);
    return s;
}

int find_output(const sfcore::json& j, const std::string& node_id) {
    for (size_t i = 0; i < j["outputs"].size(); ++i) {
        if (static_cast<std::string>(j["outputs"][i]["nodeId"]) == node_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int find_route(const sfcore::json& j, int out_idx, const std::string& source_id) {
    const sfcore::json& routes = j["outputs"][out_idx]["routes"];
    for (size_t i = 0; i < routes.size(); ++i) {
        if (static_cast<std::string>(routes[i]["sourceId"]) == source_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool array_has(const sfcore::json& arr, const std::string& id) {
    for (size_t i = 0; i < arr.size(); ++i) {
        if (static_cast<std::string>(arr[i]) == id) return true;
    }
    return false;
}

}  // namespace

TEST(Mixer, EmptyGraph) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);

    sfcore::json j = eval_ok(p);
    EXPECT_EQ(j["order"].size(), 0u);
    EXPECT_EQ(j["outputs"].size(), 0u);
    EXPECT_EQ(j["muted"].size(), 0u);
    EXPECT_EQ(j["soloed"].size(), 0u);
    sf_project_destroy(p);
}

TEST(Mixer, SimpleChain) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    ASSERT_EQ(static_cast<std::string>(j["outputs"][0]["nodeId"]), c);
    ASSERT_EQ(j["outputs"][0]["routes"].size(), 1u);
    const sfcore::json& route = j["outputs"][0]["routes"][0];
    EXPECT_EQ(static_cast<std::string>(route["sourceId"]), a);
    EXPECT_NEAR(static_cast<double>(route["gainLin"]), 1.0, 1e-9);
    ASSERT_EQ(route["nodeIds"].size(), 3u);
    EXPECT_EQ(static_cast<std::string>(route["nodeIds"][0]), a);
    EXPECT_EQ(static_cast<std::string>(route["nodeIds"][1]), b);
    EXPECT_EQ(static_cast<std::string>(route["nodeIds"][2]), c);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["peakGainLin"]), 1.0, 1e-9);
    EXPECT_FALSE(static_cast<bool>(j["outputs"][0]["clipped"]));
    sf_project_destroy(p);
}

TEST(Mixer, GainProduct) {
    // -6 dB + -6 dB down a 2-edge chain: 10^(-12/20) = 0.2512 (gains multiply).
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, a, -6.0, 0.0, 0, 0), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, b, -6.0, 0.0, 0, 0), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    ASSERT_EQ(j["outputs"][0]["routes"].size(), 1u);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["routes"][0]["gainLin"]),
                0.2512, 1e-3);
    sf_project_destroy(p);
}

TEST(Mixer, MutedNodeBlocks) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, b, 0.0, 0.0, 1, 0), SF_OK);  // muted
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);

    sfcore::json j = eval_ok(p);
    EXPECT_TRUE(array_has(j["muted"], b));
    ASSERT_EQ(j["outputs"].size(), 1u);
    EXPECT_EQ(j["outputs"][0]["routes"].size(), 0u);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["peakGainLin"]), 0.0, 1e-9);
    EXPECT_FALSE(static_cast<bool>(j["outputs"][0]["clipped"]));
    sf_project_destroy(p);
}

TEST(Mixer, SoloRestrictsRouting) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char s1[37], s2[37], o1[37], o2[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s1", s1), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s2", s2), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "o1", o1), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "o2", o2), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s1, o1, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s2, o2, 0, 0, e2), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, s1, 0.0, 0.0, 0, 1), SF_OK);  // solo s1

    sfcore::json j = eval_ok(p);
    EXPECT_TRUE(array_has(j["soloed"], s1));
    ASSERT_EQ(j["outputs"].size(), 2u);
    const int o1_idx = find_output(j, o1);
    const int o2_idx = find_output(j, o2);
    ASSERT_GE(o1_idx, 0);
    ASSERT_GE(o2_idx, 0);
    ASSERT_GE(find_route(j, o1_idx, s1), 0);  // soloed chain routes
    EXPECT_LT(find_route(j, o2_idx, s2), 0);  // other source stays silent
    EXPECT_EQ(j["outputs"][o2_idx]["routes"].size(), 0u);
    sf_project_destroy(p);
}

TEST(Mixer, SoloBranchMergeKeepsSiblingSilent) {
    // src1 and src2 merge into one output through a shared processor; soloing
    // src1 must not pull src2's branch in (upstream walk starts from soloed
    // nodes only, §4.3).
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char s1[37], s2[37], b[37], c[37], e1[37], e2[37], e3[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s1", s1), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s2", s2), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s1, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s2, b, 0, 0, e2), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e3), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, s1, 0.0, 0.0, 0, 1), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    ASSERT_EQ(j["outputs"][0]["routes"].size(), 1u);
    EXPECT_EQ(static_cast<std::string>(j["outputs"][0]["routes"][0]["sourceId"]), s1);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["routes"][0]["gainLin"]), 1.0, 1e-9);
    sf_project_destroy(p);
}

TEST(Mixer, ClipIndicatorOnly) {
    // +12 dB -> linear gain 10^(12/20) = 3.98 > 1.0: desk indicator, no clamp.
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], c[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, a, 12.0, 0.0, 0, 0), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, c, 0, 0, e), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    ASSERT_EQ(j["outputs"][0]["routes"].size(), 1u);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["routes"][0]["gainLin"]),
                3.981, 1e-2);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["peakGainLin"]), 3.981, 1e-2);
    EXPECT_TRUE(static_cast<bool>(j["outputs"][0]["clipped"]));
    sf_project_destroy(p);
}

TEST(Mixer, ParallelPathsSum) {
    // Two parallel 0 dB paths from one source: per-route gainLin sums (1+1).
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], p1[37], p2[37], c[37], e1[37], e2[37], e3[37], e4[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "p1", p1), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "p2", p2), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, p1, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, p2, 0, 0, e2), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, p1, c, 0, 0, e3), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, p2, c, 0, 0, e4), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    ASSERT_EQ(j["outputs"][0]["routes"].size(), 1u);  // one route per source
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["routes"][0]["gainLin"]),
                2.0, 1e-9);
    EXPECT_EQ(j["outputs"][0]["routes"][0]["nodeIds"].size(), 3u);  // shortest path
    sf_project_destroy(p);
}

TEST(Mixer, NoReachableSource) {
    // An output with zero reachable sources gets an empty routes array (§4.3).
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char c[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    EXPECT_EQ(static_cast<std::string>(j["outputs"][0]["nodeId"]), c);
    EXPECT_EQ(j["outputs"][0]["routes"].size(), 0u);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["peakGainLin"]), 0.0, 1e-9);
    sf_project_destroy(p);
}

TEST(Mixer, CycleRejectsEval) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    for (int i = 1; i <= 3; ++i) {
        sfcore::SignalNode n;
        n.id = "00000000-0000-4000-8000-00000000000" + std::to_string(i);
        n.kind = SF_NODE_PROCESSOR;
        n.name = "n";
        proj->doc.signalGraph.nodes.push_back(std::move(n));
    }
    const char* ids[3] = {"00000000-0000-4000-8000-000000000001",
                          "00000000-0000-4000-8000-000000000002",
                          "00000000-0000-4000-8000-000000000003"};
    for (int i = 0; i < 3; ++i) {
        sfcore::SignalEdge e;
        e.id = "edge-" + std::to_string(i);
        e.fromNodeId = ids[i];
        e.toNodeId = ids[(i + 1) % 3];
        proj->doc.signalGraph.edges.push_back(std::move(e));
    }

    char* out = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_graph_evaluate_mixer(p, &out, &len), SF_E_SCHEMA);
    EXPECT_NE(std::string(sf_last_error(p)).find("cycle"), std::string::npos);
    sf_project_destroy(p);
}

TEST(Mixer, TopoOrderAndFlags) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["order"].size(), 3u);
    EXPECT_EQ(static_cast<std::string>(j["order"][0]), a);
    EXPECT_EQ(static_cast<std::string>(j["order"][1]), b);
    EXPECT_EQ(static_cast<std::string>(j["order"][2]), c);
    EXPECT_EQ(j["muted"].size(), 0u);
    EXPECT_EQ(j["soloed"].size(), 0u);
    sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// G3 P3 — mixing-law keys (PLAN_G3 §4.1 D1, §7.1). Additive: existing cases
// above are untouched. Multi-source graphs now aggregate coherently.
// ---------------------------------------------------------------------------

TEST(Mixer, TwoSourcesLawCoherentSum) {
    // D1: peakGainLin = SUM over ALL routed sources (coherent worst case);
    // the G2 aggregation was max over routes — two 0 dB sources expose it.
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char s1[37], s2[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s1", s1), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s2", s2), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s1, c, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s2, c, 0, 0, e2), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    ASSERT_EQ(j["outputs"][0]["routes"].size(), 2u);
    const double peak = static_cast<double>(j["outputs"][0]["peakGainLin"]);
    const double power = static_cast<double>(j["outputs"][0]["powerGainLin"]);
    const double headroom = static_cast<double>(j["outputs"][0]["headroomDb"]);
    const bool clipped = static_cast<bool>(j["outputs"][0]["clipped"]);
    EXPECT_NEAR(peak, 2.0, 1e-9);              // SUM = 1 + 1 (old max: 1)
    EXPECT_NEAR(power, std::sqrt(2.0), 1e-9);  // sqrt(1^2 + 1^2)
    EXPECT_NEAR(headroom, -20.0 * std::log10(2.0), 1e-9);
    EXPECT_TRUE(clipped);
    EXPECT_EQ(clipped, headroom < 0.0);        // refactor equivalence (D1)
    // Law bracket: max route <= power <= peak.
    EXPECT_LE(power, peak);
    EXPECT_GE(power, std::max(
        static_cast<double>(j["outputs"][0]["routes"][0]["gainLin"]),
        static_cast<double>(j["outputs"][0]["routes"][1]["gainLin"])));
    sf_project_destroy(p);
}

TEST(Mixer, TwoSourcesLawUnequalGains) {
    // 0 dB + -6 dB sources: peak = 1 + 10^(-6/20), power = sqrt(1 + g^2),
    // headroom from the coherent peak; clipped <==> headroomDb < 0.
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char s1[37], s2[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s1", s1), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "s2", s2), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, s2, -6.0, 0.0, 0, 0), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s1, c, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, s2, c, 0, 0, e2), SF_OK);

    const double g2 = std::pow(10.0, -6.0 / 20.0);
    sfcore::json j = eval_ok(p);
    const double peak = static_cast<double>(j["outputs"][0]["peakGainLin"]);
    const double power = static_cast<double>(j["outputs"][0]["powerGainLin"]);
    const double headroom = static_cast<double>(j["outputs"][0]["headroomDb"]);
    const bool clipped = static_cast<bool>(j["outputs"][0]["clipped"]);
    EXPECT_NEAR(peak, 1.0 + g2, 1e-9);
    EXPECT_NEAR(power, std::sqrt(1.0 + g2 * g2), 1e-9);
    EXPECT_NEAR(headroom, -20.0 * std::log10(1.0 + g2), 1e-9);
    EXPECT_TRUE(clipped);               // coherent peak 1.5+ > 1.0
    EXPECT_EQ(clipped, headroom < 0.0);
    sf_project_destroy(p);
}

TEST(Mixer, LawKeysPresentSingleSourceUnchanged) {
    // R-A byte-compat: single source (0 dB) — peak and power equal the route
    // gain exactly (mag = 1), headroom 0 dB, not clipped. The G2 serialized
    // peakGainLin value is preserved (identical double via the law sum).
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], c[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, c, 0, 0, e), SF_OK);

    sfcore::json j = eval_ok(p);
    const double route =
        static_cast<double>(j["outputs"][0]["routes"][0]["gainLin"]);
    EXPECT_DOUBLE_EQ(static_cast<double>(j["outputs"][0]["peakGainLin"]), route);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["powerGainLin"]), route, 1e-12);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["headroomDb"]), 0.0, 1e-9);
    EXPECT_FALSE(static_cast<bool>(j["outputs"][0]["clipped"]));
    sf_project_destroy(p);
}

TEST(Mixer, LawKeysNullOnEmptyRoutes) {
    // D1 verdict note: empty routes -> peak 0 -> headroomDb JSON null
    // (-20*log10(0) = +Inf has no meaningful number), not clipped.
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char c[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);

    sfcore::json j = eval_ok(p);
    ASSERT_EQ(j["outputs"].size(), 1u);
    EXPECT_EQ(j["outputs"][0]["routes"].size(), 0u);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["peakGainLin"]), 0.0, 1e-9);
    EXPECT_NEAR(static_cast<double>(j["outputs"][0]["powerGainLin"]), 0.0, 1e-9);
    EXPECT_TRUE(j["outputs"][0]["headroomDb"].is_null());
    EXPECT_FALSE(static_cast<bool>(j["outputs"][0]["clipped"]));
    sf_project_destroy(p);
}

TEST(Mixer, PerOutputKeysAlphabeticalWithLawKeys) {
    // R-A verdict note: the per-output dump keeps the "sorted keys per level"
    // property — new law keys interleave alphabetically:
    //   clipped, headroomDb, nodeId, peakGainLin, powerGainLin, routes.
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], c[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, c, 0, 0, e), SF_OK);

    const std::string s = eval_ok_str(p);
    const std::array<std::string, 6> keys{"\"clipped\"", "\"headroomDb\"",
                                          "\"nodeId\"", "\"peakGainLin\"",
                                          "\"powerGainLin\"", "\"routes\""};
    std::size_t prev = 0;
    for (const std::string& key : keys) {
        const std::size_t pos = s.find(key);
        ASSERT_NE(pos, std::string::npos) << "missing key " << key;
        EXPECT_LT(prev, pos) << "key " << key << " out of alphabetical order";
        prev = pos;
    }
    sf_project_destroy(p);
}