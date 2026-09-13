cmake_minimum_required(VERSION 3.22)

set(FIXTURES_DIR ${CMAKE_SOURCE_DIR}/../tests/fixtures)
set(SCHEMA_DIR ${CMAKE_SOURCE_DIR}/data/schemas)

add_executable(sf_unit_tests
    test_version.cpp
    test_project_create.cpp
    test_project_serialize.cpp
    test_schema_validate.cpp
    test_migration.cpp
    test_geometry.cpp
    test_scene_venue.cpp
    test_name_model.cpp
    test_diagnostics.cpp
    test_graph_nodes.cpp
    test_graph_edges.cpp
    test_graph_validate.cpp
    test_graph_mixer.cpp
    test_cmd_queue.cpp
)

target_include_directories(sf_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/graph
)

target_compile_definitions(sf_unit_tests PRIVATE
    FIXTURES_DIR="${FIXTURES_DIR}"
    SCHEMA_DIR="${SCHEMA_DIR}"
)

target_link_libraries(sf_unit_tests PRIVATE
    sfcore
    gtest_main
    stdc++fs
)

include(GoogleTest)
gtest_discover_tests(sf_unit_tests)
