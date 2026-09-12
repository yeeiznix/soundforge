#include <gtest/gtest.h>

#include "soundforge/sf_migration.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_schema.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

TEST(Migration, V0ToV2Validates) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v0.json");
    size_t cap = s.size() + 65536;
    std::vector<char> buf(cap);
    std::memcpy(buf.data(), s.data(), s.size());
    buf[s.size()] = '\0';
    size_t len = s.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, cap, 0, 2), SF_OK);
    std::string out(buf.data(), len);
    EXPECT_NE(out.find("\"schemaVersion\": 2"), std::string::npos);
    EXPECT_NE(out.find("\"migrated 0->1\""), std::string::npos);
    EXPECT_NE(out.find("\"migrated 1->2\""), std::string::npos);
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(buf.data(), len, err, sizeof(err)), SF_OK) << err;
}

TEST(Migration, V1ToV2InjectsRoomDefaults) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v1.json");
    size_t cap = s.size() + 65536;
    std::vector<char> buf(cap);
    std::memcpy(buf.data(), s.data(), s.size());
    buf[s.size()] = '\0';
    size_t len = s.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, cap, 1, 2), SF_OK);
    std::string out(buf.data(), len);
    EXPECT_NE(out.find("\"schemaVersion\": 2"), std::string::npos);
    EXPECT_NE(out.find("\"widthM\": 12.0"), std::string::npos);
    EXPECT_NE(out.find("\"center\""), std::string::npos);
    EXPECT_NE(out.find("\"x\": 6.0"), std::string::npos);
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(buf.data(), len, err, sizeof(err)), SF_OK) << err;
}

TEST(Migration, V0ToV1SchemaVersionOne) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v0.json");
    size_t cap = s.size() + 65536;
    std::vector<char> buf(cap);
    std::memcpy(buf.data(), s.data(), s.size());
    size_t len = s.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, cap, 0, 1), SF_OK);
    std::string out(buf.data(), len);
    EXPECT_NE(out.find("\"schemaVersion\": 1"), std::string::npos);
    EXPECT_NE(out.find("\"action\": \"project.migrate\""), std::string::npos);
}

TEST(Migration, IdempotentOneToOne) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v1.json");
    size_t cap = s.size() + 65536;
    std::vector<char> buf(cap);
    std::memcpy(buf.data(), s.data(), s.size());
    size_t len = s.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, cap, 1, 1), SF_OK);
}

TEST(Migration, DowngradeRejected) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v1.json");
    size_t cap = s.size() + 65536;
    std::vector<char> buf(cap);
    std::memcpy(buf.data(), s.data(), s.size());
    size_t len = s.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, cap, 1, 0), SF_E_VERSION);
}

TEST(Migration, InsufficientCapReturnsNoMem) {
    std::string s = read_file(FIXTURES_DIR "/project_minimal_v0.json");
    size_t cap = s.size() + 65536;
    std::vector<char> buf(cap);
    std::memcpy(buf.data(), s.data(), s.size());
    size_t len = s.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, 16, 0, 1), SF_E_NOMEM);
}
