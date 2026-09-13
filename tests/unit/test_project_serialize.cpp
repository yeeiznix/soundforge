#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_graph.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_version.h"

#include <string>

TEST(ProjectSerialize, RoundTripPreservesId) {
    sf_project_t* p = sf_project_create("RoundTrip", nullptr);
    ASSERT_NE(p, nullptr);
    std::string id0 = sf_project_get_id(p);
    char* json = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    ASSERT_NE(json, nullptr);
    sf_project_destroy(p);

    sf_project_t* p2 = nullptr;
    EXPECT_EQ(sf_project_from_json(json, len, &p2), SF_OK);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(sf_project_get_id(p2), id0);
    EXPECT_EQ(sf_project_get_schema_version(p2), 2);
    sf_project_destroy(p2);
    sf_free_string(json);
}

TEST(ProjectSerialize, RequiredKeysPresent) {
    sf_project_t* p = sf_project_create("Keys", nullptr);
    ASSERT_NE(p, nullptr);
    char* json = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    std::string s(json, len);
    EXPECT_NE(s.find("\"schemaVersion\""), std::string::npos);
    EXPECT_NE(s.find("\"engineVersion\""), std::string::npos);
    EXPECT_NE(s.find("\"project\""), std::string::npos);
    EXPECT_NE(s.find("\"venue\""), std::string::npos);
    EXPECT_NE(s.find("\"scene\""), std::string::npos);
    EXPECT_NE(s.find("\"signalGraph\""), std::string::npos);
    EXPECT_NE(s.find("\"powerGraph\""), std::string::npos);
    EXPECT_NE(s.find("\"auditLog\""), std::string::npos);
    sf_free_string(json);
    sf_project_destroy(p);
}

TEST(ProjectSerialize, GraphMixerAndEdgeIdsSurviveRoundTrip) {
    // G2 P7 wire fix: node mixers carry gainDb/pan and edges carry their id,
    // so mixer values persist across save/reopen and removeEdge can target
    // edges by id (before this, gainDb/pan and edge ids were dropped).
    sf_project_t* p = sf_project_create("Graph", nullptr);
    ASSERT_NE(p, nullptr);
    char src[37], out[37], edge[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", src), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", out), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, src, out, 0, 0, edge), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, src, 6.0, -0.5, 0, 0), SF_OK);

    char* json = nullptr;
    size_t len = 0;
    ASSERT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    sf_project_destroy(p);

    sf_project_t* p2 = nullptr;
    ASSERT_EQ(sf_project_from_json(json, len, &p2), SF_OK);
    sf_free_string(json);

    char* json2 = nullptr;
    size_t len2 = 0;
    ASSERT_EQ(sf_project_to_json(p2, &json2, &len2), SF_OK);
    const sfcore::json j = sfcore::json::parse(std::string(json2, len2));
    sf_free_string(json2);

    const auto& nodes = j["signalGraph"]["nodes"];
    const auto& edges = j["signalGraph"]["edges"];
    ASSERT_EQ(nodes.size(), 2u);
    ASSERT_EQ(edges.size(), 1u);
    // Insertion order: src first, then out.
    EXPECT_NEAR(static_cast<double>(nodes[0]["mixer"]["gainDb"]), 6.0, 1e-9);
    EXPECT_NEAR(static_cast<double>(nodes[0]["mixer"]["pan"]), -0.5, 1e-9);
    EXPECT_EQ(static_cast<std::string>(edges[0]["id"]), edge);
    sf_project_destroy(p2);
}
