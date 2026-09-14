#include <gtest/gtest.h>

#include "soundforge/sf_project.h"
#include "soundforge/sf_schema.h"
#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

TEST(SchemaValidate, MinimalV2Passes) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v2.json");
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_OK)
        << "err=" << err;
}

TEST(SchemaValidate, FullEmptyV2Passes) {
    std::string s = read_file(FIXTURES_DIR "/project_full_empty_v2.json");
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_OK)
        << "err=" << err;
}

TEST(SchemaValidate, CorruptFails) {
    std::string s = read_file(FIXTURES_DIR "/project_corrupt.json");
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_GT(std::strlen(err), 0u);
}

TEST(SchemaValidate, GoldenSchemaExists) {
    std::string s = read_file(SCHEMA_DIR "/project_schema.json");
    EXPECT_GT(s.size(), 0u);
}

TEST(SchemaValidate, ValidateSignalGraphValid) {
    std::string s = read_file(FIXTURES_DIR "/project_signalgraph_v2.json");
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_OK)
        << "err=" << err;
}

TEST(SchemaValidate, ValidateSignalGraphCorrupt) {
    std::string s = read_file(FIXTURES_DIR "/project_graph_corrupt.json");
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_GT(std::strlen(err), 0u);
    // Should report at least 4 errors: bad kind, missing nodeId, dangling edge, cycle
    std::string err_str(err);
    EXPECT_TRUE(err_str.find("4 error") != std::string::npos || 
                err_str.find("5 error") != std::string::npos ||
                err_str.find("6 error") != std::string::npos) 
        << "Expected 4+ errors, got: " << err;
}

TEST(SchemaValidate, CheckGraphNodeValidation) {
    // Inline test for bad node kind
    std::string json_bad_kind = R"({
        "schemaVersion": 2,
        "engineVersion": "0.1.0-g1",
        "project": {
            "id": "550e8400-e29b-41d4-a716-446655440000",
            "name": "Test", "createdAt": "2026-09-11T00:00:00.000Z",
            "modifiedAt": "2026-09-11T00:00:00.000Z", "author": "", "notes": ""
        },
        "venue": {
            "id": "660e8400-e29b-41d4-a716-446655440000", "name": "Test",
            "dimensions": {"widthM": 12.0, "depthM": 10.0, "heightM": 4.0}
        },
        "scene": {
            "id": "770e8400-e29b-41d4-a716-446655440000", "name": "Test",
            "venueRef": "660e8400-e29b-41d4-a716-446655440000",
            "geometry": {
                "center": {"x": 6.0, "y": 5.0, "z": 2.0},
                "listening": {"x": 6.0, "y": 5.0, "z": 2.0}
            }
        },
        "audienceReceivers": [], "equipment": [],
        "signalGraph": {
            "nodes": [{
                "kind": "bad_kind",
                "id": "a10e8400-e29b-41d4-a716-446655440001",
                "label": "Test", "position": {"x": 0.0, "y": 0.0},
                "mixer": {"mute": false, "solo": false}
            }],
            "edges": []
        },
        "powerGraph": {"nodes": [], "edges": []},
        "audioAssets": [], "dspPresets": [], "arrayConfigurations": [],
        "measurements": [], "simulationRuns": [], "trainingScenarios": [],
        "inventoryRefs": [], "reports": [], "auditLog": []
    })";
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(json_bad_kind.data(), json_bad_kind.size(), err, sizeof(err)), SF_E_SCHEMA);
    std::string err_str(err);
    EXPECT_TRUE(err_str.find("invalid kind") != std::string::npos) << "Expected 'invalid kind' error, got: " << err;
}

TEST(SchemaValidate, CheckGraphEdgeDangling) {
    // Inline test for edge referencing non-existent node
    std::string json_dangling = R"({
        "schemaVersion": 2, "engineVersion": "0.1.0-g1",
        "project": {
            "id": "550e8400-e29b-41d4-a716-446655440000",
            "name": "Test", "createdAt": "2026-09-11T00:00:00.000Z",
            "modifiedAt": "2026-09-11T00:00:00.000Z", "author": "", "notes": ""
        },
        "venue": {
            "id": "660e8400-e29b-41d4-a716-446655440000", "name": "Test",
            "dimensions": {"widthM": 12.0, "depthM": 10.0, "heightM": 4.0}
        },
        "scene": {
            "id": "770e8400-e29b-41d4-a716-446655440000", "name": "Test",
            "venueRef": "660e8400-e29b-41d4-a716-446655440000",
            "geometry": {
                "center": {"x": 6.0, "y": 5.0, "z": 2.0},
                "listening": {"x": 6.0, "y": 5.0, "z": 2.0}
            }
        },
        "audienceReceivers": [], "equipment": [],
        "signalGraph": {
            "nodes": [{
                "kind": "source", "id": "a10e8400-e29b-41d4-a716-446655440001",
                "label": "Test", "position": {"x": 0.0, "y": 0.0},
                "mixer": {"mute": false, "solo": false}
            }],
            "edges": [{
                "from": "a10e8400-e29b-41d4-a716-446655440001",
                "to": "a10e8400-e29b-41d4-a716-446655440099",
                "label": "Dangling"
            }]
        },
        "powerGraph": {"nodes": [], "edges": []},
        "audioAssets": [], "dspPresets": [], "arrayConfigurations": [],
        "measurements": [], "simulationRuns": [], "trainingScenarios": [],
        "inventoryRefs": [], "reports": [], "auditLog": []
    })";
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(json_dangling.data(), json_dangling.size(), err, sizeof(err)), SF_E_SCHEMA);
    std::string err_str(err);
    EXPECT_TRUE(err_str.find("unknown node") != std::string::npos) << "Expected 'unknown node' error, got: " << err;
}

TEST(SchemaValidate, DspchainV2Passes) {
    // G3 P5: the DspChain fixture carries dspPresetRef on the wire (string on
    // the processor, null on the source, key absent on the output) — the
    // refreshed schema must accept all three forms.
    std::string s = read_file(FIXTURES_DIR "/project_dspchain_v2.json");
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_OK)
        << "err=" << err;
}

TEST(SchemaValidate, DspPresetRefNullOrStringOk) {
    // SEC-G3-10: null and "" both mean "none" and always type-check; a
    // non-empty string ref type-checks too (existence is a graph-validate /
    // health-check concern, not a schema concern).
    std::string s = read_file(FIXTURES_DIR "/project_dspchain_v2.json");
    nlohmann::json doc = nlohmann::json::parse(s);
    char err[512] = {0};
    doc["signalGraph"]["nodes"][0]["dspPresetRef"] = nullptr;
    const std::string d1 = doc.dump();
    EXPECT_EQ(sf_validate_project_json(d1.data(), d1.size(), err, sizeof(err)), SF_OK)
        << "err=" << err;
    doc["signalGraph"]["nodes"][0]["dspPresetRef"] = "";
    const std::string d2 = doc.dump();
    EXPECT_EQ(sf_validate_project_json(d2.data(), d2.size(), err, sizeof(err)), SF_OK)
        << "err=" << err;
}

TEST(SchemaValidate, DspPresetRefNumberRejected) {
    // SEC-G3-10: a number-typed ref is rejected by the native validator —
    // parity with the Python mirror (test_schema_py.py asserts the same).
    std::string s = read_file(FIXTURES_DIR "/project_dspchain_v2.json");
    nlohmann::json doc = nlohmann::json::parse(s);
    doc["signalGraph"]["nodes"][1]["dspPresetRef"] = 3;
    const std::string d = doc.dump();
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(d.data(), d.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_NE(std::string(err).find("dspPresetRef"), std::string::npos)
        << "Expected 'dspPresetRef' error, got: " << err;
}

// G3 P7 depth coupling: the validator now pre-parses nesting depth via
// checked_parse(). A 256-deep doc passes the gate and reaches the SCHEMA
// verdict; only >256 trips the dedicated pre-reject text.
static std::string nested_arrays(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += "[";
    for (int i = 0; i < n; ++i) s += "]";
    return s;
}

TEST(SchemaValidate, DepthGate256PassesToSchema257PreRejected) {
    const std::string d256 = nested_arrays(256);
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(d256.data(), d256.size(), err, sizeof(err)), SF_E_SCHEMA);
    // The depth gate did NOT trip (err is a schema/parse verdict, not "depth").
    EXPECT_EQ(std::string(err).find("json depth"), std::string::npos) << err;
    EXPECT_NE(std::string(err).find("root: expected JSON object"), std::string::npos) << err;

    const std::string d257 = nested_arrays(257);
    EXPECT_EQ(sf_validate_project_json(d257.data(), d257.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_EQ(std::string(err), "schema: json depth exceeds 256");
}
