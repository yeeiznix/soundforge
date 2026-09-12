#include <gtest/gtest.h>

#include "soundforge/sf_project.h"
#include "soundforge/sf_schema.h"

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
