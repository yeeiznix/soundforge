#include <gtest/gtest.h>

#include "soundforge/sf_diagnostics.h"
#include "soundforge/sf_project.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

TEST(Diagnostics, LogLevelsAreStored) {
    sf_log(SF_LOG_DEBUG, "test", "debug msg");
    sf_log(SF_LOG_INFO, "test", "info msg");
    sf_log(SF_LOG_WARN, "test", "warn msg");
    sf_log(SF_LOG_ERROR, "test", "error msg");
    std::filesystem::path log_dir = std::filesystem::temp_directory_path() / "soundforge_logs";
    std::filesystem::create_directories(log_dir);
    std::filesystem::path log = log_dir / "sf.log";
    EXPECT_EQ(sf_flush_logs(log.string().c_str()), SF_OK);
    EXPECT_TRUE(std::filesystem::exists(log));
    std::ifstream f(log);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_NE(s.find("debug msg"), std::string::npos);
    EXPECT_NE(s.find("info msg"), std::string::npos);
    EXPECT_NE(s.find("warn msg"), std::string::npos);
    EXPECT_NE(s.find("error msg"), std::string::npos);
}

TEST(Diagnostics, LastErrorAfterError) {
    sf_project_create("", nullptr);
    EXPECT_STREQ(sf_last_error_global(), "project.create: name must be non-empty");
}

TEST(Diagnostics, ThreadLocalIsolation) {
    std::string t1_err, t2_err;
    std::thread t1([&] {
        sf_project_create("", nullptr);
        t1_err = sf_last_error_global();
    });
    std::thread t2([&] {
        sf_project_create("", nullptr);
        t2_err = sf_last_error_global();
    });
    t1.join();
    t2.join();
    EXPECT_EQ(t1_err, "project.create: name must be non-empty");
    EXPECT_EQ(t2_err, "project.create: name must be non-empty");
}
