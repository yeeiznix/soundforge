cmake_minimum_required(VERSION 3.22)

add_executable(sf_integration_tests
    test_project_io.cpp
    test_kotlin_bridge_jni.cpp
)

target_include_directories(sf_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/src/core
)

target_compile_definitions(sf_integration_tests PRIVATE
    FIXTURES_DIR="${CMAKE_SOURCE_DIR}/../tests/fixtures"
)

target_link_libraries(sf_integration_tests PRIVATE
    sfcore
    gtest_main
    stdc++fs
)

include(GoogleTest)
gtest_discover_tests(sf_integration_tests)
