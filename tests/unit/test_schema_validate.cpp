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

TEST(SchemaValidate, MinimalV1Passes) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v1.json");
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_OK)
        << "err=" << err;
}

TEST(SchemaValidate, FullEmptyV1Passes) {
    std::string s = read_file(FIXTURES_DIR "/project_full_empty_v1.json");
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
