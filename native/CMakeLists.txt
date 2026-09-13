cmake_minimum_required(VERSION 3.22)
project(SoundForgeNative CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Version stamp (default; CI may override with -DSF_BUILD_VERSION="0.1.0-g3+build.123")
# Gate history (PLAN_G3 §5.3): the planned G2 "-g2" bump never landed; G3 lands
# "-g3" directly (G2 cache/default were left at "-g1").
set(SF_BUILD_VERSION "0.1.0-g3" CACHE STRING "Engine version string")

option(SF_BUILD_TESTS "Build native unit/integration tests" ON)

add_subdirectory(src/core)

# Stub module directories — add_subdirectory keeps them visible and parsing;
# G0 only provides INTERFACE targets; implementations arrive in later gates.
foreach(m graph dsp audio acoustics arrays power render measurement optimization)
  add_subdirectory(src/${m})
endforeach()

if (SF_BUILD_TESTS)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()