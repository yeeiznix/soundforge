#include <gtest/gtest.h>

#include "soundforge/sf_project.h"
#include "soundforge/sf_diagnostics.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

class ProjectIoTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_dir;

    void SetUp() override {
        tmp_dir = std::filesystem::temp_directory_path() / "sf_g0_tests";
        std::filesystem::remove_all(tmp_dir);
        std::filesystem::create_directories(tmp_dir);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }
};

TEST_F(ProjectIoTest, CreateSaveOpenReopen) {
    sf_project_t* p = sf_project_create("IntTest", nullptr);
    ASSERT_NE(p, nullptr);
    std::string id0 = sf_project_get_id(p);
    std::filesystem::path path = tmp_dir / "test.sfproj";
    EXPECT_EQ(sf_project_save_to_path(p, path.c_str()), SF_OK);
    sf_project_destroy(p);

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    sf_project_t* p2 = nullptr;
    EXPECT_EQ(sf_project_open_from_path(path.c_str(), &p2), SF_OK);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(sf_project_get_id(p2), id0);

    char report[1024] = {0};
    EXPECT_EQ(sf_project_health_check(p2, report, sizeof(report)), SF_OK);
    EXPECT_NE(std::string(report).find("ok"), std::string::npos);

    sf_project_destroy(p2);
}

TEST_F(ProjectIoTest, CorruptOpenFails) {
    std::filesystem::path path = tmp_dir / "corrupt.sfproj";
    {
        std::ofstream f(path, std::ios::binary);
        f << "{\"schemaVersion\":1}";
    }
    sf_project_t* p = nullptr;
    EXPECT_EQ(sf_project_open_from_path(path.c_str(), &p), SF_E_SCHEMA);
    EXPECT_EQ(p, nullptr);
}

TEST_F(ProjectIoTest, MigrationViaFile) {
    std::string v0 = R"({"schemaVersion":0,"project":{"id":"550e8400-e29b-41d4-a716-446655440000","name":"MigrateMe"}})";
    std::filesystem::path path = tmp_dir / "migrate.sfproj";
    {
        std::ofstream f(path, std::ios::binary);
        f << v0;
    }
    sf_project_t* p = nullptr;
    EXPECT_EQ(sf_project_open_from_path(path.c_str(), &p), SF_OK);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_project_get_schema_version(p), 2);
    sf_project_destroy(p);

    std::filesystem::path bak = path;
    bak += ".bak.v0";
    EXPECT_TRUE(std::filesystem::exists(bak));
}
