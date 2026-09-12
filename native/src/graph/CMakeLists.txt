cmake_minimum_required(VERSION 3.22)

# G0: stub library sfgraph (empty) — satisfies Appendix A without logic
add_library(sfgraph INTERFACE)
target_include_directories(sfgraph INTERFACE ${CMAKE_SOURCE_DIR}/include)
