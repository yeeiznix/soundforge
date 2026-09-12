#include <gtest/gtest.h>

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
    EXPECT_EQ(sf_project_get_schema_version(p2), 1);
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
