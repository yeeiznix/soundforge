cmake_minimum_required(VERSION 3.22)

# G2 P2/P4: typed graph core + routing engine (STATIC library, pure helpers)
add_library(sfgraph STATIC graph.cpp routing.cpp)

# Match core's include style
target_include_directories(sfgraph
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

# Core requirements
set_target_properties(sfgraph PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

find_package(Threads REQUIRED)
target_link_libraries(sfgraph PUBLIC Threads::Threads)
