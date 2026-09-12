// SoundForge G1 — scene/venue editing mutators (PLAN_G1 §4.2).
#include <gtest/gtest.h>

#include "soundforge/sf_project.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

static std::string json_of(sf_project_t* p) {
    char* out = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_project_to_json(p, &out, &len), SF_OK);
    std::string s(out ? out : "", len);
    if (out) sf_free_string(out);
    return s;
}

TEST(SceneVenue, RenameProject) {
    sf_project_t* p = sf_project_create("Before", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_project_rename(p, "After"), SF_OK);
    EXPECT_STREQ(sf_project_get_name(p), "After");
    std::string s = json_of(p);
    EXPECT_NE(s.find("\"action\": \"project.rename\""), std::string::npos);
    EXPECT_NE(s.find("\"name\": \"After\""), std::string::npos);
    sf_project_destroy(p);
}

TEST(SceneVenue, RenameRejects) {
    sf_project_t* p = sf_project_create("R", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_project_rename(nullptr, "X"), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_project_rename(p, ""), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("non-empty"), std::string::npos);
    std::string long_name(201, 'a');
    EXPECT_EQ(sf_project_rename(p, long_name.c_str()), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("too long"), std::string::npos);
    EXPECT_STREQ(sf_project_get_name(p), "R");  // unchanged on failure
    sf_project_destroy(p);
}

TEST(SceneVenue, VenueRename) {
    sf_project_t* p = sf_project_create("V", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_venue_rename(p, "Grand Hall"), SF_OK);
    std::string s = json_of(p);
    EXPECT_NE(s.find("\"action\": \"venue.update\""), std::string::npos);
    EXPECT_NE(s.find("\"name\": \"Grand Hall\""), std::string::npos);
    EXPECT_EQ(sf_venue_rename(p, ""), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_venue_rename(nullptr, "X"), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(SceneVenue, VenueSetDimensions) {
    sf_project_t* p = sf_project_create("D", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_venue_set_dimensions(p, 15.0, 12.0, 6.0), SF_OK);
    std::string s = json_of(p);
    EXPECT_NE(s.find("\"widthM\": 15.0"), std::string::npos);
    EXPECT_NE(s.find("\"depthM\": 12.0"), std::string::npos);
    EXPECT_NE(s.find("\"heightM\": 6.0"), std::string::npos);
    EXPECT_EQ(sf_venue_set_dimensions(p, 0.0, 12.0, 6.0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_venue_set_dimensions(p, 15.0, -2.0, 6.0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_venue_set_dimensions(nullptr, 15.0, 12.0, 6.0), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(SceneVenue, ShrinkDimsClampsGeometry) {
    sf_project_t* p = sf_project_create("C", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_scene_set_geometry(p, 6.0, 5.0, 2.0, 6.0, 5.0, 2.0), SF_OK);
    EXPECT_EQ(sf_venue_set_dimensions(p, 4.0, 3.0, 2.0), SF_OK);
    std::string s = json_of(p);
    EXPECT_NE(s.find("\"x\": 4.0"), std::string::npos);  // clamped from 6
    EXPECT_NE(s.find("\"y\": 3.0"), std::string::npos);  // clamped from 5
    EXPECT_EQ(sf_scene_set_geometry(p, 6.0, 5.0, 2.0, 6.0, 5.0, 2.0), SF_E_SCHEMA);
    sf_project_destroy(p);
}

TEST(SceneVenue, SceneSetGeometry) {
    sf_project_t* p = sf_project_create("G", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_scene_set_geometry(p, 3.0, 4.0, 1.5, 6.0, 5.0, 2.0), SF_OK);
    std::string s = json_of(p);
    EXPECT_NE(s.find("\"action\": \"scene.update\""), std::string::npos);
    EXPECT_NE(s.find("\"z\": 1.5"), std::string::npos);
    EXPECT_EQ(sf_scene_set_geometry(p, 13.0, 5.0, 2.0, 6.0, 5.0, 2.0), SF_E_SCHEMA);
    EXPECT_NE(std::string(sf_last_error(p)).find("outside venue bounds"),
              std::string::npos);
    EXPECT_EQ(sf_scene_set_geometry(p, 6.0, 5.0, 2.0, 0.0, -1.0, 2.0), SF_E_SCHEMA);
    EXPECT_EQ(sf_scene_set_geometry(nullptr, 6.0, 5.0, 2.0, 6.0, 5.0, 2.0),
              SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(SceneVenue, ModifiedAtBumpedOnMutate) {
    sf_project_t* p = sf_project_create("M", nullptr);
    ASSERT_NE(p, nullptr);
    auto extract = [](const std::string& body, const char* field) {
        const std::string key = std::string("\"") + field + "\": \"";
        const size_t at = body.find(key);
        if (at == std::string::npos) return std::string();
        return body.substr(at + key.size(), 24);
    };
    const std::string before = extract(json_of(p), "modifiedAt");
    EXPECT_FALSE(before.empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // avoid same-ms collision
    sf_project_rename(p, "M2");
    const std::string after = extract(json_of(p), "modifiedAt");
    EXPECT_NE(before, after);
    sf_project_destroy(p);
}

TEST(SceneVenue, RoundTripPreservesEdits) {
    sf_project_t* p = sf_project_create("RT", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_venue_set_dimensions(p, 20.0, 15.0, 8.0), SF_OK);
    EXPECT_EQ(sf_scene_set_geometry(p, 5.0, 3.0, 1.5, 10.0, 12.0, 4.0), SF_OK);
    char* json = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    ASSERT_NE(json, nullptr);
    sf_project_destroy(p);

    sf_project_t* p2 = nullptr;
    EXPECT_EQ(sf_project_from_json(json, len, &p2), SF_OK);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(sf_project_get_schema_version(p2), 2);
    std::string s = json_of(p2);
    EXPECT_NE(s.find("\"widthM\": 20.0"), std::string::npos);
    EXPECT_NE(s.find("\"y\": 12.0"), std::string::npos);  // listening point preserved
    sf_project_destroy(p2);
    sf_free_string(json);
}

TEST(SceneVenue, HealthWarnsOnLegacyOutOfBoundsGeometry) {
    // Legacy/hand-edited docs can carry scene geometry outside the venue box;
    // the schema validator is structural only so they still open, and the
    // health check reports a Warning (not Error) — PLAN_G1 §9.6.
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v2.json");
    const std::string orig_center =
        "      \"center\": {\n"
        "        \"x\": 6.0,\n"
        "        \"y\": 5.0,\n"
        "        \"z\": 2.0\n"
        "      },";
    const std::string oob_center =
        "      \"center\": {\n"
        "        \"x\": 100.0,\n"
        "        \"y\": 100.0,\n"
        "        \"z\": 100.0\n"
        "      },";
    ASSERT_NE(s.find(orig_center), std::string::npos);
    s.replace(s.find(orig_center), orig_center.size(), oob_center);

    sf_project_t* p = nullptr;
    EXPECT_EQ(sf_project_from_json(s.data(), s.size(), &p), SF_OK);  // still opens
    ASSERT_NE(p, nullptr);

    char buf[4096];
    const sf_result_t rc = sf_project_health_check(p, buf, sizeof(buf));
    const std::string r(buf);
    EXPECT_EQ(rc, SF_E_SCHEMA);  // non-ok report return convention
    EXPECT_NE(r.find("\"status\": \"warning\""), std::string::npos);
    EXPECT_EQ(r.find("\"status\": \"error\""), std::string::npos);
    EXPECT_NE(r.find("outside venue bounds"), std::string::npos);
    sf_project_destroy(p);
}