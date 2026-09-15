cmake_minimum_required(VERSION 3.22)

# G4 P4 (PLAN_G4 §6 P4 / §1.2 "Audio engine" row): the host audio engine.
#
# G0 shipped `sfaudio` as an INTERFACE stub ("satisfies Appendix A without
# logic"); P4 replaces it with the real STATIC library.
#
# Dependency edges (PLAN_G4 §2 link discipline — no cycles):
#   sfaudio -> sfcore     (SfProject handle, log/diagnostics, PlanSnapshotStore,
#                          the internal sf_queue_runner + RunnerObserver)
#   sfaudio -> sfgraph    (dsp::RenderPlan / render_chain_planned)
#   sfaudio -> sfmeasure  (sfmeasure::TruePeak)
# sfcore -> sfgraph -> sfdsp: all acyclic; nothing links back into sfaudio.
#
# The 11 sf_audio_engine_* exports are host-only C ABI (PLAN_G4 §4.5 D5): they
# are NOT wired to JNI, so the JNI export grep stays 26.
add_library(sfaudio STATIC
    audio_engine.cpp
)

target_include_directories(sfaudio
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core        # sf_internal.hpp, snapshot.hpp
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../graph       # render_plan.hpp, graph_internal.hpp
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../dsp         # dsp_internal.hpp (AudioBlock)
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../measurement # true_peak.hpp
)

set_target_properties(sfaudio PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

find_package(Threads REQUIRED)
target_link_libraries(sfaudio
    PRIVATE sfcore
    PRIVATE sfgraph
    PRIVATE sfmeasure
    PUBLIC Threads::Threads
)
