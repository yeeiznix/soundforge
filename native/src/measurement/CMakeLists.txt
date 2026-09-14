cmake_minimum_required(VERSION 3.22)

# sfmeasure (renamed from G0 INTERFACE stub `sfmeasurement` — PLAN_G4 §6 P2,
# B-1): leaf true-peak measurement module.
#
# Leaf rule (PLAN_G4 B-3):
#   * depends on nothing but the C/C++ runtime; std=c++20 provides <cmath>,
#     <cstdint> — no external or SoundForge targets;
#   * the hot process path (process/reset) performs NO heap allocation,
#     NO locks, NO syscalls — only reads/writes caller buffers + fixed state;
#   * raw `float* const*` channels — no AudioBlock, no sfdsp, no sfcore.
add_library(sfmeasure STATIC
    true_peak.cpp
)
target_include_directories(sfmeasure PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}   # true_peak.hpp
)