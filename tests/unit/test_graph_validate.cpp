// SoundForge G2 P4 — graph validate / topological order C ABI (PLAN_G2 §4.1–4.3).
// Valid graphs are built through the mutators; invalid graphs (duplicate ids,
// dangling edges, cycles) are injected directly into the doc so validate()
// has something to report — mutators reject them at write time.
#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_graph.h"

#include <string>

namespace {

constexpr size_t REPORT_CAP = 16384;

sf_result_t validate_into(sf_project_t* p, std::string& report) {
    char buf[REPORT_CAP];
    const sf_result_t rc = sf_graph_validate(p, buf, sizeof(buf));
    report = std::string(buf);
    return rc;
}

bool report_has(const std::string& report, const std::string& needle) {
    return report.find(needle) != std::string::npos;
}

// Inject a node straight into the doc (bypasses mutator validation).
void push_node(sfcore::SfProject* proj, int kind, const char* id) {
    sfcore::SignalNode n;
    n.id = id;
    n.kind = kind;
    n.name = "n";
    proj->doc.signalGraph.nodes.push_back(std::move(n));
}

// Inject an edge straight into the doc (the mutator would reject these).
void push_edge(sfcore::SfProject* proj, const char* from, const char* to, const char* id) {
    sfcore::SignalEdge e;
    e.id = id;
    e.fromNodeId = from;
    e.toNodeId = to;
    proj->doc.signalGraph.edges.push_back(std::move(e));
}

// Deterministic 36-char UUID-shaped ids (v4 nibbles) for injected nodes.
const char ID_A[] = "00000000-0000-4000-8000-000000000001";
const char ID_B[] = "00000000-0000-4000-8000-000000000002";
const char ID_C[] = "00000000-0000-4000-8000-000000000003";

}  // namespace

TEST(GraphValidate, CleanGraph) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_OK);
    EXPECT_TRUE(report_has(report, "\"status\": \"ok\""));
    EXPECT_TRUE(report_has(report, "\"errors\": []"));
    EXPECT_TRUE(report_has(report, "\"nodes\": 3"));
    EXPECT_TRUE(report_has(report, "\"edges\": 2"));
    sf_project_destroy(p);
}

TEST(GraphValidate, DuplicateNodeId) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    push_node(proj, SF_NODE_SOURCE, ID_A);
    push_node(proj, SF_NODE_PROCESSOR, ID_A);  // duplicate id

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_E_SCHEMA);
    EXPECT_TRUE(report_has(report, "\"status\": \"error\""));
    EXPECT_TRUE(report_has(report, "duplicate node id"));
    sf_project_destroy(p);
}

TEST(GraphValidate, DanglingEdge) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    push_node(proj, SF_NODE_SOURCE, ID_A);
    push_edge(proj, ID_A, "00000000-0000-4000-8000-0000000000ff", "edge-1");

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_E_SCHEMA);
    EXPECT_TRUE(report_has(report, "\"status\": \"error\""));
    EXPECT_TRUE(report_has(report, "dangling toNodeId"));
    sf_project_destroy(p);
}

TEST(GraphValidate, EdgeIntoSource) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    push_node(proj, SF_NODE_SOURCE, ID_A);
    push_node(proj, SF_NODE_SOURCE, ID_B);
    push_edge(proj, ID_A, ID_B, "edge-1");

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_E_SCHEMA);
    EXPECT_TRUE(report_has(report, "routes into source"));
    sf_project_destroy(p);
}

TEST(GraphValidate, EdgeOutOfOutput) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    push_node(proj, SF_NODE_OUTPUT, ID_A);
    push_node(proj, SF_NODE_OUTPUT, ID_B);
    push_edge(proj, ID_A, ID_B, "edge-1");

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_E_SCHEMA);
    EXPECT_TRUE(report_has(report, "routes out of output"));
    sf_project_destroy(p);
}

TEST(GraphValidate, Cycle) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    push_node(proj, SF_NODE_PROCESSOR, ID_A);
    push_node(proj, SF_NODE_PROCESSOR, ID_B);
    push_node(proj, SF_NODE_PROCESSOR, ID_C);
    push_edge(proj, ID_A, ID_B, "edge-1");
    push_edge(proj, ID_B, ID_C, "edge-2");
    push_edge(proj, ID_C, ID_A, "edge-3");

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_E_SCHEMA);
    EXPECT_TRUE(report_has(report, "cycle detected"));
    sf_project_destroy(p);
}

TEST(GraphValidate, OrphanIsWarning) {
    // §3.3: a node unreachable from any source is a Warning, not an Error —
    // it stays openable and repairable in the editor.
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "orphan", b), SF_OK);

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_E_SCHEMA);  // non-ok report convention
    EXPECT_TRUE(report_has(report, "\"status\": \"warning\""));
    EXPECT_TRUE(report_has(report, "not reachable from any source"));
    EXPECT_TRUE(report_has(report, "\"errors\": []"));
    sf_project_destroy(p);
}

TEST(GraphValidate, Stats) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", b), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e), SF_OK);

    std::string report;
    EXPECT_EQ(validate_into(p, report), SF_OK);
    EXPECT_TRUE(report_has(report, "\"nodes\": 2"));
    EXPECT_TRUE(report_has(report, "\"edges\": 1"));
    sf_project_destroy(p);
}

TEST(GraphValidate, TopologicalOrder) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);

    char* json = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_graph_topological_order(p, &json, &len), SF_OK);
    ASSERT_NE(json, nullptr);
    sfcore::json j = sfcore::json::parse(std::string(json, len));
    sf_free_string(json);
    ASSERT_EQ(j["order"].size(), 3u);
    EXPECT_EQ(static_cast<std::string>(j["order"][0]), a);
    EXPECT_EQ(static_cast<std::string>(j["order"][1]), b);
    EXPECT_EQ(static_cast<std::string>(j["order"][2]), c);
    sf_project_destroy(p);
}

TEST(GraphValidate, TopologicalOrderCycle) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    push_node(proj, SF_NODE_PROCESSOR, ID_A);
    push_node(proj, SF_NODE_PROCESSOR, ID_B);
    push_edge(proj, ID_A, ID_B, "edge-1");
    push_edge(proj, ID_B, ID_A, "edge-2");

    char* json = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_graph_topological_order(p, &json, &len), SF_E_SCHEMA);
    EXPECT_NE(std::string(sf_last_error(p)).find("cycle"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphValidate, NullArgsRejected) {
    char buf[256];
    char* json = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_graph_validate(nullptr, buf, sizeof(buf)), SF_E_INVALID_ARG);

    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_graph_validate(p, nullptr, 64), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_validate(p, buf, 0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_topological_order(nullptr, &json, &len), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_topological_order(p, nullptr, &len), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_evaluate_mixer(nullptr, &json, &len), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_evaluate_mixer(p, &json, nullptr), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}