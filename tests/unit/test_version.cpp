#include <gtest/gtest.h>

#include "soundforge/sf_version.h"

TEST(Version, EngineVersionMatchesMacro) {
    EXPECT_STREQ(sf_engine_version(), SF_ENGINE_VERSION);
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
