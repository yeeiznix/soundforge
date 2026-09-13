// SoundForge G2 P3 — graph node mutators (PLAN_G2 §3.3, §4.2).
#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_graph.h"

#include <regex>
#include <string>

namespace {

std::string json_of(sf_project_t* p) {
    char* out = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_project_to_json(p, &out, &len), SF_OK);
    std::string s(out ? out : "", len);
    if (out) sf_free_string(out);
    return s;
}

bool looks_like_uuid(const char* id) {
    static const std::regex re(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return std::regex_match(id, re);
}

}  // namespace

TEST(GraphNodes, AddNodeEachKind) {
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    char id[37];

    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", id), SF_OK);
    EXPECT_TRUE(looks_like_uuid(id));
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", id), SF_OK);
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", id), SF_OK);
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_BUS, "bus", id), SF_OK);

    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    ASSERT_EQ(proj->doc.signalGraph.nodes.size(), 4u);
    EXPECT_EQ(proj->doc.signalGraph.nodes[0].kind, sfcore::SfNodeSource);
    EXPECT_EQ(proj->doc.signalGraph.nodes[1].kind, sfcore::SfNodeProcessor);
    EXPECT_EQ(proj->doc.signalGraph.nodes[2].kind, sfcore::SfNodeOutput);
    EXPECT_EQ(proj->doc.signalGraph.nodes[3].kind, sfcore::SfNodeBus);
    EXPECT_EQ(proj->doc.signalGraph.nodes[0].name, "src");
    EXPECT_TRUE(proj->doc.signalGraph.nodes[0].dspPresetRef.empty());
    sf_project_destroy(p);
}

TEST(GraphNodes, AddNodeRejectsBadKind) {
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    char id[37];
    EXPECT_EQ(sf_graph_add_node(p, 0, "src", id), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_add_node(p, 5, "src", id), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_add_node(p, -1, "src", id), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("invalid kind"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphNodes, AddNodeRejectsBadName) {
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    char id[37];
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "", id), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, nullptr, id), SF_E_INVALID_ARG);
    std::string long_name(65, 'a');
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, long_name.c_str(), id), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find(">64 chars"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphNodes, AddNodeLabelMultibyteCodePointCap) {
    // G3 P1: label cap is 64 code points (byte ceiling 256), not 64 bytes.
    // A 32-code-point multibyte label (96 bytes > 64) is now accepted.
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    char id[37];
    std::string label32;
    for (int i = 0; i < 32; ++i) label32 += "\xE2\x82\xAC";  // € (3 bytes each)
    ASSERT_GT(label32.size(), 64u);
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, label32.c_str(), id), SF_OK);

    // 65 code points (195 bytes) rejected with the updated message.
    std::string label65;
    for (int i = 0; i < 65; ++i) label65 += "\xE2\x82\xAC";
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, label65.c_str(), id),
              SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find(">64 chars"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphNodes, AddNodeRejectsNullArgs) {
    char id[37];
    // null project
    EXPECT_EQ(sf_graph_add_node(nullptr, SF_NODE_SOURCE, "a", id), SF_E_INVALID_ARG);
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    // null out_id
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "a", nullptr), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(GraphNodes, RemoveNodeCascadesEdges) {
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "A", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "B", b), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e), SF_OK);

    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    ASSERT_EQ(proj->doc.signalGraph.edges.size(), 1u);

    EXPECT_EQ(sf_graph_remove_node(p, a), SF_OK);
    EXPECT_EQ(proj->doc.signalGraph.nodes.size(), 1u);
    EXPECT_EQ(proj->doc.signalGraph.edges.size(), 0u);
    sf_project_destroy(p);
}

TEST(GraphNodes, RemoveNodeNotFound) {
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_graph_remove_node(p, "00000000-0000-4000-8000-000000000000"),
              SF_E_NOT_FOUND);
    EXPECT_NE(std::string(sf_last_error(p)).find("not found"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphNodes, RemoveNodeRecordsAudit) {
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "A", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "B", b), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e), SF_OK);
    ASSERT_EQ(sf_graph_remove_node(p, a), SF_OK);

    std::string s = json_of(p);
    EXPECT_NE(s.find("\"action\": \"graph.removeNode\""), std::string::npos);
    EXPECT_NE(s.find("cascaded=1"), std::string::npos);
    sf_project_destroy(p);
}