# SoundForge G0 — native core library (sfcore).
# Links: nlohmann/json (vendored single header), Threads.

set(SFCORE_SOURCES
    version.cpp
    uuid.cpp
    diagnostics.cpp
    geometry.cpp
    schema.cpp
    json_codec.cpp
    migration.cpp
    project.cpp
    command_queue.cpp
    command_queue_thread.cpp
    snapshot.cpp
    ../graph/graph_abi.cpp
)

add_library(sfcore STATIC ${SFCORE_SOURCES})

# Graph module lives in a sibling directory configured later (top-level
# foreach). Forward target references resolve at generate time; the explicit
# include dir is belt-and-suspenders so `#include "graph_internal.hpp"`
# resolves from core sources regardless of link ordering.
# snapshot.cpp compiles RenderPlan values, so it also needs the dsp headers
# (render_plan.hpp -> dsp_internal.hpp).
target_include_directories(sfcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../graph)
target_include_directories(sfcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../dsp)
target_link_libraries(sfcore PRIVATE sfgraph)

# graph_abi.cpp lives in src/graph/ but is compiled into sfcore (it needs
# sfcore internals). Make sure sf_internal.hpp is findable from that TU.
target_include_directories(sfcore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

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