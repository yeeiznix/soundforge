// SoundForge G2 P3 — graph edge mutators and mixer/preset (PLAN_G2 §3.3, §4.2).
#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_graph.h"

#include <cmath>
#include <cstring>
#include <limits>
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

void add_source_output(sf_project_t* p, char* src, char* out) {
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "src", src), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "out", out), SF_OK);
}

void add_proc(sf_project_t* p, const char* name, char* id) {
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, name, id), SF_OK);
}

}  // namespace

// --- Edge tests ---

TEST(GraphEdges, AddRemoveEdge) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    add_source_output(p, a, b);

    EXPECT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e), SF_OK);
    EXPECT_TRUE(looks_like_uuid(e));

    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    ASSERT_EQ(proj->doc.signalGraph.edges.size(), 1u);
    EXPECT_EQ(proj->doc.signalGraph.edges[0].fromNodeId, a);
    EXPECT_EQ(proj->doc.signalGraph.edges[0].toNodeId, b);

    EXPECT_EQ(sf_graph_remove_edge(p, e), SF_OK);
    EXPECT_EQ(proj->doc.signalGraph.edges.size(), 0u);
    sf_project_destroy(p);
}

TEST(GraphEdges, SelfLoopRejected) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, a, a, 0, 0, e), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("self-loop"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, DuplicateEdgeRejected) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e1[37], e2[37];
    add_source_output(p, a, b);

    EXPECT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e2), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("duplicate"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, EdgeIntoSourceRejected) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "A", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "B", b), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("cannot route into a source"),
              std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, EdgeOutOfOutputRejected) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "A", a), SF_OK);
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "B", b), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("cannot route out of an output"),
              std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, CycleRejected) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], c[37], e1[37], e2[37], e3[37];
    add_proc(p, "A", a);
    add_proc(p, "B", b);
    add_proc(p, "C", c);

    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e1), SF_OK);
    ASSERT_EQ(sf_graph_add_edge(p, b, c, 0, 0, e2), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, c, a, 0, 0, e3), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("would create cycle"),
              std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, RemoveEdgeNotFound) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_graph_remove_edge(p, "00000000-0000-4000-8000-000000000000"),
              SF_E_NOT_FOUND);
    EXPECT_NE(std::string(sf_last_error(p)).find("not found"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, FromNodeNotFound) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char b[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_OUTPUT, "B", b), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, "00000000-0000-4000-8000-000000000000", b, 0, 0, e),
              SF_E_NOT_FOUND);
    EXPECT_NE(std::string(sf_last_error(p)).find("from node"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, ToNodeNotFound) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], e[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "A", a), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, a, "00000000-0000-4000-8000-000000000000", 0, 0, e),
              SF_E_NOT_FOUND);
    EXPECT_NE(std::string(sf_last_error(p)).find("to node"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, RemoveEdgeHasAudit) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    add_source_output(p, a, b);
    ASSERT_EQ(sf_graph_add_edge(p, a, b, 0, 0, e), SF_OK);
    ASSERT_EQ(sf_graph_remove_edge(p, e), SF_OK);
    EXPECT_NE(json_of(p).find("\"action\": \"graph.removeEdge\""), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, NullArgsRejected) {
    char e[37];
    EXPECT_EQ(sf_graph_add_edge(nullptr, "a", "b", 0, 0, e), SF_E_INVALID_ARG);
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "A", a), SF_OK);
    EXPECT_EQ(sf_graph_add_edge(p, nullptr, a, 0, 0, e), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_add_edge(p, a, nullptr, 0, 0, e), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_add_edge(p, a, a, 0, 0, nullptr), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(GraphEdges, NegativePortsRejected) {
    sf_project_t* p = sf_project_create("E", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37], b[37], e[37];
    add_source_output(p, a, b);
    EXPECT_EQ(sf_graph_add_edge(p, a, b, -1, 0, e), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_add_edge(p, a, b, 0, -1, e), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("port must be >= 0"), std::string::npos);
    sf_project_destroy(p);
}

// --- Mixer tests ---

TEST(GraphEdges, SetMixerValid) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    EXPECT_EQ(sf_graph_set_mixer(p, a, 3.0, 0.5, 1, 0), SF_OK);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    auto& node = proj->doc.signalGraph.nodes[0];
    EXPECT_DOUBLE_EQ(node.mixer.gainDb, 3.0);
    EXPECT_DOUBLE_EQ(node.mixer.pan, 0.5);
    EXPECT_TRUE(node.mixer.mute);
    EXPECT_FALSE(node.mixer.solo);
    sf_project_destroy(p);
}

TEST(GraphEdges, SetMixerBoundaryValues) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    EXPECT_EQ(sf_graph_set_mixer(p, a, -60.0, -1.0, 0, 0), SF_OK);
    EXPECT_EQ(sf_graph_set_mixer(p, a, 24.0, 1.0, 0, 0), SF_OK);
    EXPECT_EQ(sf_graph_set_mixer(p, a, 0.0, 0.0, 0, 0), SF_OK);
    sf_project_destroy(p);
}

TEST(GraphEdges, SetMixerRejectsBadGain) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    EXPECT_EQ(sf_graph_set_mixer(p, a, 24.1, 0.0, 0, 0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_set_mixer(p, a, -60.1, 0.0, 0, 0), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("invalid gain_db"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, SetMixerRejectsNonFiniteGain) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    EXPECT_EQ(sf_graph_set_mixer(p, a, std::nan(""), 0.0, 0, 0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_set_mixer(p, a, std::numeric_limits<double>::infinity(),
                                 0.0, 0, 0),
              SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(GraphEdges, SetMixerRejectsBadPan) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    EXPECT_EQ(sf_graph_set_mixer(p, a, 0.0, 1.1, 0, 0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_set_mixer(p, a, 0.0, -1.1, 0, 0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_graph_set_mixer(p, a, 0.0, std::nan(""), 0, 0), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("invalid pan"), std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, SetMixerNotFound) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_graph_set_mixer(p, "00000000-0000-4000-8000-000000000000",
                                 0.0, 0.0, 0, 0),
              SF_E_NOT_FOUND);
    sf_project_destroy(p);
}

TEST(GraphEdges, SetMixerAudit) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    ASSERT_EQ(sf_graph_set_mixer(p, a, 3.0, 0.5, 1, 1), SF_OK);
    std::string s = json_of(p);
    EXPECT_NE(s.find("\"action\": \"graph.setMixer\""), std::string::npos);
    EXPECT_NE(s.find("gain=3"), std::string::npos);
    sf_project_destroy(p);
}

// --- Preset tests ---

static void add_dsp_preset(sf_project_t* p, const char* id, const char* name) {
    // Add an ObjectEnvelope to dspPresets by round-tripping through JSON
    // The C ABI does not have a direct "add preset" mutator yet, so we
    // manipulate the doc via the internal struct pointer.
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    sfcore::ObjectEnvelope env;
    env.id = id;
    env.type = "dspPreset";
    env.data["name"] = name;
    proj->doc.dspPresets.push_back(env);
}

TEST(GraphEdges, SetPresetValid) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    add_dsp_preset(p, "a1b2c3d4-e5f6-4000-8000-111111111111", "EQ Rock");
    EXPECT_EQ(sf_graph_set_preset(p, a, "a1b2c3d4-e5f6-4000-8000-111111111111"),
              SF_OK);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    EXPECT_EQ(proj->doc.signalGraph.nodes[0].dspPresetRef,
              "a1b2c3d4-e5f6-4000-8000-111111111111");
    sf_project_destroy(p);
}

TEST(GraphEdges, SetPresetNotFound) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);
    EXPECT_EQ(sf_graph_set_preset(p, a, "00000000-0000-4000-8000-000000000000"),
              SF_E_NOT_FOUND);
    EXPECT_NE(std::string(sf_last_error(p)).find("preset not found"),
              std::string::npos);
    sf_project_destroy(p);
}

TEST(GraphEdges, SetPresetClear) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    add_dsp_preset(p, "a1b2c3d4-e5f6-4000-8000-111111111111", "EQ Rock");
    ASSERT_EQ(sf_graph_set_preset(p, a, "a1b2c3d4-e5f6-4000-8000-111111111111"),
              SF_OK);
    // Clear with empty string
    EXPECT_EQ(sf_graph_set_preset(p, a, ""), SF_OK);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    EXPECT_TRUE(proj->doc.signalGraph.nodes[0].dspPresetRef.empty());
    sf_project_destroy(p);
}

TEST(GraphEdges, SetPresetNullClears) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    add_dsp_preset(p, "a1b2c3d4-e5f6-4000-8000-111111111111", "EQ Rock");
    ASSERT_EQ(sf_graph_set_preset(p, a, "a1b2c3d4-e5f6-4000-8000-111111111111"),
              SF_OK);
    EXPECT_EQ(sf_graph_set_preset(p, a, nullptr), SF_OK);
    auto* proj = reinterpret_cast<sfcore::SfProject*>(p);
    EXPECT_TRUE(proj->doc.signalGraph.nodes[0].dspPresetRef.empty());
    sf_project_destroy(p);
}

TEST(GraphEdges, SetPresetAudit) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    char a[37];
    ASSERT_EQ(sf_graph_add_node(p, SF_NODE_PROCESSOR, "A", a), SF_OK);

    add_dsp_preset(p, "a1b2c3d4-e5f6-4000-8000-111111111111", "EQ Rock");
    ASSERT_EQ(sf_graph_set_preset(p, a, "a1b2c3d4-e5f6-4000-8000-111111111111"),
              SF_OK);
    EXPECT_NE(json_of(p).find("\"action\": \"graph.setPreset\""), std::string::npos);
    sf_project_destroy(p);
}