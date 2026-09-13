cmake_minimum_required(VERSION 3.22)

# G2 P2/P4: typed graph core + routing engine (STATIC library, pure helpers)
# G3 P3: evaluate_mixer calls the sfcore::dsp mixing law (PLAN_G3 D1/D2);
#        dsp_render.cpp = the chain-render harness, compiled INTO sfgraph (§4.2).
add_library(sfgraph STATIC graph.cpp routing.cpp dsp_render.cpp)

# Match core's include style
target_include_directories(sfgraph
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../dsp  # dsp_internal.hpp (sfcore::dsp law)
)

# Core requirements
set_target_properties(sfgraph PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

find_package(Threads REQUIRED)
target_link_libraries(sfgraph PUBLIC Threads::Threads)

# Mixing law lives in sfdsp (PLAN_G3 §4.2). Acyclic: graph -> dsp -> nothing.
target_link_libraries(sfgraph PRIVATE sfdsp)