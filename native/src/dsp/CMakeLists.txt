cmake_minimum_required(VERSION 3.22)

# G3 P2 (D1/D2): sfdsp STATIC library — mixing law + finite-block kernels.
add_library(sfdsp STATIC law.cpp kernels.cpp)

# Internal header lives beside the sources; keep it PRIVATE (nothing in this
# library's API surface is public — no C ABI, no JNI).
target_include_directories(sfdsp
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(sfdsp PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)

# Leaf library: no dependencies (ORC-P2-001 — law/kernels use zero sfcore
# symbols; keeps the link graph acyclic: graph -> dsp -> nothing).