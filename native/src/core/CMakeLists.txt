# SoundForge G0 — native core library (sfcore).
# Links: nlohmann/json (vendored single header), Threads.

set(SFCORE_SOURCES
    version.cpp
    uuid.cpp
    diagnostics.cpp
    schema.cpp
    json_codec.cpp
    migration.cpp
    project.cpp
)

add_library(sfcore STATIC ${SFCORE_SOURCES})

# Generate build-stamped version header into THIS directory so include order
# is deterministic (version_gen.h.in lives next to this CMakeLists.txt).
configure_file(version_gen.h.in ${CMAKE_CURRENT_BINARY_DIR}/version_gen.h @ONLY)

target_include_directories(sfcore
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}  # for version_gen.h
)

target_compile_features(sfcore PRIVATE cxx_std_20)

# pthread on Linux/Android
find_package(Threads REQUIRED)
target_link_libraries(sfcore PUBLIC Threads::Threads)