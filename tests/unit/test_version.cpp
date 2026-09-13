#include <gtest/gtest.h>

#include <string>

#include "soundforge/sf_version.h"

TEST(Version, EngineVersionMatchesMacro) {
    EXPECT_STREQ(sf_engine_version(), SF_ENGINE_VERSION);
    // G3 P1 (ORC-P1-5): do not hard-pin the exact stamp — CMake may override
    // with -DSF_BUILD_VERSION="0.1.0-g3+build.123". Assert the G3 base prefix.
    EXPECT_EQ(std::string(sf_engine_version()).rfind("0.1.0-", 0), 0u);
}

TEST(Version, SchemaVersionIsTwo) {
    EXPECT_EQ(sf_schema_version(), SF_SCHEMA_VERSION);
    EXPECT_EQ(sf_schema_version(), 2);
}

TEST(Version, Compatibility) {
    EXPECT_EQ(sf_is_compatible(0), 1);
    EXPECT_EQ(sf_is_compatible(1), 1);
    EXPECT_EQ(sf_is_compatible(2), 1);
    EXPECT_EQ(sf_is_compatible(3), 0);
    EXPECT_EQ(sf_is_compatible(-1), 0);
}
