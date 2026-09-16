cmake_minimum_required(VERSION 3.22)
project(SoundForgeNative CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Version stamp (default; CI may override with -DSF_BUILD_VERSION="0.1.0-g5+build.123")
# Gate history (PLAN_G5 §4.2): the planned G2 "-g2" bump never landed; G3 lands
# "-g3" directly (G2 cache/default were left at "-g1"); G4 lands "-g4"; G5 lands "-g5".
set(SF_BUILD_VERSION "0.1.0-g5" CACHE STRING "Engine version string")

option(SF_BUILD_TESTS "Build native unit/integration tests" ON)
option(SF_BUILD_HOST_SHARED "Build host shared libsoundforge.so (Python ctypes layer)" ON)

add_subdirectory(src/core)

# Stub module directories — add_subdirectory keeps them visible and parsing;
# G0 only provides INTERFACE targets; implementations arrive in later gates.
foreach(m graph dsp audio acoustics arrays power render measurement optimization)
  add_subdirectory(src/${m})
endforeach()

# G6 — host-only shared library for the Python ctypes parity layer
# (PLAN_G6.md §3.2). "Host-only" = NOT a cross-compilation.
# CMAKE_CROSSCOMPILING is the reliable signal: it is TRUE only under explicit
# cross intent (toolchain file or -DCMAKE_SYSTEM_NAME=...). ANDROID is NOT
# usable as a guard signal — this proot environment reports ANDROID=1 /
# CMAKE_SYSTEM_NAME=Android with native gcc and no toolchain file, while
# CROSS=FALSE (ORC-G6-18). Edge accepted: an NDK build from an Android host
# also reports CROSS=FALSE and would build this target — acceptable, the app
# CMake (app/src/main/cpp) never add_subdirectory's this tree, and on an
# Android host that .so IS the correct host lib for the ctypes layer.
if(SF_BUILD_HOST_SHARED AND NOT CMAKE_CROSSCOMPILING)
  add_subdirectory(src/host)
endif()

if (SF_BUILD_TESTS)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()