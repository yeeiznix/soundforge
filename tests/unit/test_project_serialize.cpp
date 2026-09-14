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
    // G3 P5: the same round trip also proves dspPresetRef survives.
    sf_project_t* p = sf_project_create("Graph", nullptr);
    ASSERT_NE(p, nullptr);
    char src[37], out[37], edge[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", src), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", out), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, src, out, 0, 0, edge), SF_OK);
    ASSERT_EQ(sf_graph_set_mixer(p, src, 6.0, -0.5, 0, 0), SF_OK);

    // G3 P5: attach a preset envelope and set it on the source node.
    const char kPreset[] = "b20e8400-e29b-41d4-a716-44665544ab01";
    {
        auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
        sfcore::ObjectEnvelope env;
        env.id = kPreset;
        env.type = "dspPreset";
        env.createdAt = "2026-09-13T00:00:00.000Z";
        env.modifiedAt = "2026-09-13T00:00:00.000Z";
        env.provenance = "created";
        env.data["name"] = "EQ Rock";
        proj->doc.dspPresets.push_back(env);
    }
    ASSERT_EQ(sf_graph_set_preset(p, src, kPreset), SF_OK);

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
    // G3 P5: the preset attachment crossed the wire and came back equal.
    EXPECT_EQ(static_cast<std::string>(nodes[0]["dspPresetRef"]), kPreset);
    // The unset node serializes its "" as null ("" <-> null symmetry).
    EXPECT_TRUE(nodes[1]["dspPresetRef"].is_null());
    sf_project_destroy(p2);
}

TEST(ProjectSerialize, DspPresetRefLegacyDocOpensEmpty) {
    // G3 P5: a pre-wire-refresh doc WITHOUT the dspPresetRef key must still
    // open (tolerant codec read) and read ""; re-serialization emits JSON
    // null ("" <-> null symmetry on the wire).
    sf_project_t* p = sf_project_create("Legacy", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    char* json = nullptr;
    size_t len = 0;
    ASSERT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    sfcore::json j = sfcore::json::parse(std::string(json, len));
    ASSERT_TRUE(j["signalGraph"]["nodes"][0].contains("dspPresetRef"));
    j["signalGraph"]["nodes"][0].erase("dspPresetRef");  // legacy doc
    const std::string legacy = j.dump();
    sf_free_string(json);
    sf_project_destroy(p);

    sf_project_t* p2 = nullptr;
    ASSERT_EQ(sf_project_from_json(legacy.data(), legacy.size(), &p2), SF_OK);
    ASSERT_NE(p2, nullptr);
    auto* proj2 = reinterpret_cast<sfcore::SfProject*>(p2);
    ASSERT_EQ(proj2->doc.signalGraph.nodes.size(), 1u);
    EXPECT_TRUE(proj2->doc.signalGraph.nodes[0].dspPresetRef.empty());

    char* json2 = nullptr;
    size_t len2 = 0;
    ASSERT_EQ(sf_project_to_json(p2, &json2, &len2), SF_OK);
    const std::string s2(json2, len2);
    sf_free_string(json2);
    EXPECT_NE(s2.find("\"dspPresetRef\": null"), std::string::npos);
    sf_project_destroy(p2);
}

TEST(ProjectSerialize, DanglingDspPresetRefFlaggedByValidateAndHealth) {
    // SEC-G3-9 (D6-amd): a non-empty dspPresetRef that resolves to no
    // dspPresets[].id is an error in sf_graph_validate AND
    // sf_project_health_check (load stays tolerant — the dangling edge
    // precedent). ""-means-none must NOT trip the check (equals
    // sf_graph_set_preset(p, n, "") clear semantics). Wire-level: the ref
    // crosses to_json -> from_json first.
    sf_project_t* p = sf_project_create("Dangling", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "proc", b), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", c), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);

    const char kPreset[] = "b20e8400-e29b-41d4-a716-44665544ab01";
    {
        auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
        sfcore::ObjectEnvelope env;
        env.id = kPreset;
        env.type = "dspPreset";
        env.createdAt = "2026-09-13T00:00:00.000Z";
        env.modifiedAt = "2026-09-13T00:00:00.000Z";
        env.provenance = "created";
        env.data["name"] = "EQ Rock";
        proj->doc.dspPresets.push_back(env);
    }
    ASSERT_EQ(sf_graph_set_preset(p, b, kPreset), SF_OK);

    // Cross the wire: save -> reopen. The ref must survive.
    char* json = nullptr;
    size_t len = 0;
    ASSERT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    sf_project_destroy(p);
    sf_project_t* p2 = nullptr;
    ASSERT_EQ(sf_project_from_json(json, len, &p2), SF_OK);
    sf_free_string(json);

    char report[16384] = {0};
    // Resolvable ref -> both reports clean.
    EXPECT_EQ(sf_graph_validate(p2, report, sizeof(report)), SF_OK) << report;
    EXPECT_EQ(sf_project_health_check(p2, report, sizeof(report)), SF_OK) << report;

    // Drop the preset envelope in-memory -> the ref now dangles.
    auto* proj2 = reinterpret_cast<sfcore::SfProject*>(p2);
    proj2->doc.dspPresets.clear();

    EXPECT_EQ(sf_graph_validate(p2, report, sizeof(report)), SF_E_SCHEMA);
    EXPECT_NE(std::string(report).find("dangling dspPresetRef"), std::string::npos) << report;
    EXPECT_EQ(sf_project_health_check(p2, report, sizeof(report)), SF_E_SCHEMA);
    EXPECT_NE(std::string(report).find("dspPresetRef"), std::string::npos) << report;

    // ""-means-none: clear via the mutator -> no ref error -> reports clean.
    ASSERT_EQ(sf_graph_set_preset(p2, b, ""), SF_OK);
    EXPECT_EQ(sf_graph_validate(p2, report, sizeof(report)), SF_OK) << report;
    EXPECT_EQ(sf_project_health_check(p2, report, sizeof(report)), SF_OK) << report;
    sf_project_destroy(p2);
}
