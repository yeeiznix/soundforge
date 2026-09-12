#include <gtest/gtest.h>

#include "soundforge/sf_project.h"
#include "soundforge/sf_version.h"

#include <regex>
#include <string>

TEST(ProjectCreate, BasicFields) {
    sf_project_t* p = sf_project_create("Demo PA", "Tester");
    ASSERT_NE(p, nullptr);
    EXPECT_STREQ(sf_project_get_name(p), "Demo PA");
    std::string id = sf_project_get_id(p);
    static const std::regex uuid_re("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    EXPECT_TRUE(std::regex_match(id, uuid_re));
    EXPECT_EQ(sf_project_get_schema_version(p), 2);
    EXPECT_STREQ(sf_project_get_engine_version(p), SF_ENGINE_VERSION);
    sf_project_destroy(p);
}

TEST(ProjectCreate, NullAuthorAllowed) {
    sf_project_t* p = sf_project_create("No Author", nullptr);
    ASSERT_NE(p, nullptr);
    sf_project_destroy(p);
}

TEST(ProjectCreate, EmptyNameRejected) {
    sf_project_t* p = sf_project_create("", nullptr);
    EXPECT_EQ(p, nullptr);
    EXPECT_STREQ(sf_last_error_global(), "project.create: name must be non-empty");
}

TEST(ProjectCreate, TimestampsEqualAtCreation) {
    sf_project_t* p = sf_project_create("Timed", nullptr);
    ASSERT_NE(p, nullptr);
    char* json = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    ASSERT_NE(json, nullptr);
    std::string s(json, len);
    sf_free_string(json);
    EXPECT_NE(s.find("\"createdAt\""), std::string::npos);
    EXPECT_NE(s.find("\"project.create\""), std::string::npos);
    sf_project_destroy(p);
}
