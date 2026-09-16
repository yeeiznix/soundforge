# SoundForge G0 — Android native build (app/src/main/cpp).
#
# Produces the final shared library libsfcore.so containing jni_bridge.cpp +
# the portable sfcore core sources, so System.loadLibrary("sfcore") resolves.
# The desktop/native tree builds sfcore as a STATIC lib in its own CMake
# project (native/src/core/CMakeLists.txt); we do NOT add_subdirectory it —
# we compile the same sources directly to avoid a target-name clash and to
# keep tests out of the app build.
cmake_minimum_required(VERSION 3.22)
project(SoundForgeAppNative CXX)

set(NATIVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native)

set(SFCORE_SOURCES
    ${NATIVE_ROOT}/src/core/version.cpp
    ${NATIVE_ROOT}/src/core/uuid.cpp
    ${NATIVE_ROOT}/src/core/diagnostics.cpp
    ${NATIVE_ROOT}/src/core/geometry.cpp
    ${NATIVE_ROOT}/src/core/schema.cpp
    ${NATIVE_ROOT}/src/core/json_codec.cpp
    ${NATIVE_ROOT}/src/core/migration.cpp
    ${NATIVE_ROOT}/src/core/project.cpp
    ${NATIVE_ROOT}/src/core/command_queue.cpp
    ${NATIVE_ROOT}/src/core/command_queue_thread.cpp
    ${NATIVE_ROOT}/src/core/snapshot.cpp
    ${NATIVE_ROOT}/src/graph/graph_abi.cpp
    ${NATIVE_ROOT}/src/graph/graph.cpp
    ${NATIVE_ROOT}/src/graph/routing.cpp
    ${NATIVE_ROOT}/src/graph/dsp_render.cpp
    ${NATIVE_ROOT}/src/graph/render_plan.cpp
    ${NATIVE_ROOT}/src/dsp/law.cpp
    ${NATIVE_ROOT}/src/dsp/kernels.cpp
)

# Version stamp — mirrors native/CMakeLists.txt (CI may override
# -DSF_BUILD_VERSION="0.1.0-g0+build.123").
set(SF_BUILD_VERSION "0.1.0-g0" CACHE STRING "Engine version string")
configure_file(${NATIVE_ROOT}/src/core/version_gen.h.in ${CMAKE_CURRENT_BINARY_DIR}/version_gen.h)

add_library(sfcore SHARED
    jni_bridge.cpp
    ${SFCORE_SOURCES}
)

target_include_directories(sfcore
    PRIVATE ${NATIVE_ROOT}/include
    PRIVATE ${NATIVE_ROOT}/third_party   # nlohmann/json single header
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}  # version_gen.h
    PRIVATE ${NATIVE_ROOT}/src/core      # sf_internal.hpp (graph_abi.cpp TU)
    PRIVATE ${NATIVE_ROOT}/src/graph     # graph_internal.hpp, render_plan.hpp
    PRIVATE ${NATIVE_ROOT}/src/dsp       # dsp_internal.hpp (RenderPlan/AudioBlock)
)

target_compile_features(sfcore PRIVATE cxx_std_20)
set_target_properties(sfcore PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# sfcore uses exceptions (ABI boundary catches) and RTTI via nlohmann/json;
# the NDK toolchain disables both by default.
target_compile_options(sfcore PRIVATE -fexceptions -frtti)

# project.cpp calls fsync() (POSIX) on !_WIN32. glibc exposes it through
# transitively-included headers on desktop; Android Bionic does not, so the
# NDK build fails with "undeclared identifier 'fsync'". Force the standard
# header at compile level instead of touching the frozen native source.
target_compile_options(sfcore PRIVATE -include unistd.h)

find_package(Threads REQUIRED)  # std::thread in command_queue_thread.cpp
find_library(android-log-lib log)
target_link_libraries(sfcore PRIVATE ${android-log-lib} Threads::Threads)
