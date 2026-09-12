#include <gtest/gtest.h>

#include <limits>

#include "soundforge/sf_geometry.h"

TEST(Geometry, ValidateBox) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(sf_geo_validate_box(12.0, 10.0, 4.0), SF_OK);
    EXPECT_EQ(sf_geo_validate_box(1.0, 1.0, 1.0), SF_OK);
    EXPECT_EQ(sf_geo_validate_box(0.0, 10.0, 4.0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_geo_validate_box(12.0, -1.0, 4.0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_geo_validate_box(12.0, 10.0, 0.0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_geo_validate_box(nan, 10.0, 4.0), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_geo_validate_box(inf, 10.0, 4.0), SF_E_INVALID_ARG);
}

TEST(Geometry, PointInBox) {
    int in = -1;
    EXPECT_EQ(sf_geo_point_in_box(0.0, 0.0, 0.0, 12.0, 10.0, 4.0, &in), SF_OK);
    EXPECT_EQ(in, 1);  // inclusive corner
    EXPECT_EQ(sf_geo_point_in_box(12.0, 10.0, 4.0, 12.0, 10.0, 4.0, &in), SF_OK);
    EXPECT_EQ(in, 1);  // opposite inclusive corner
    EXPECT_EQ(sf_geo_point_in_box(6.0, 5.0, 2.0, 12.0, 10.0, 4.0, &in), SF_OK);
    EXPECT_EQ(in, 1);  // room middle
    EXPECT_EQ(sf_geo_point_in_box(12.01, 5.0, 2.0, 12.0, 10.0, 4.0, &in), SF_OK);
    EXPECT_EQ(in, 0);
    EXPECT_EQ(sf_geo_point_in_box(6.0, -0.01, 2.0, 12.0, 10.0, 4.0, &in), SF_OK);
    EXPECT_EQ(in, 0);
    EXPECT_EQ(sf_geo_point_in_box(6.0, 5.0, 2.0, 12.0, 10.0, 4.0, nullptr),
              SF_E_INVALID_ARG);
    EXPECT_EQ(sf_geo_point_in_box(6.0, 5.0, 2.0, 0.0, 10.0, 4.0, &in),
              SF_E_INVALID_ARG);
}

TEST(Geometry, Distance) {
    double m = -1.0;
    EXPECT_EQ(sf_geo_distance(0.0, 0.0, 0.0, 3.0, 4.0, 0.0, &m), SF_OK);
    EXPECT_NEAR(m, 5.0, 1e-9);
    EXPECT_EQ(sf_geo_distance(1.0, 2.0, 3.0, 1.0, 2.0, 4.0, &m), SF_OK);
    EXPECT_NEAR(m, 1.0, 1e-9);
    EXPECT_EQ(sf_geo_distance(0.0, 0.0, 0.0, 3.0, 4.0, 0.0, nullptr),
              SF_E_INVALID_ARG);
}

TEST(Geometry, MaxDimension) {
    double m = -1.0;
    EXPECT_EQ(sf_geo_max_dimension(12.0, 10.0, 4.0, &m), SF_OK);
    EXPECT_DOUBLE_EQ(m, 12.0);
    EXPECT_EQ(sf_geo_max_dimension(3.0, 30.0, 4.0, &m), SF_OK);
    EXPECT_DOUBLE_EQ(m, 30.0);
    EXPECT_EQ(sf_geo_max_dimension(12.0, 10.0, 4.0, nullptr), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_geo_max_dimension(0.0, 10.0, 4.0, &m), SF_E_INVALID_ARG);
}