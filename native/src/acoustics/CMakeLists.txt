cmake_minimum_required(VERSION 3.22)

# G0: stub library sfacoustics — satisfies Appendix A without logic
add_library(sfacoustics INTERFACE)
target_include_directories(sfacoustics INTERFACE ${CMAKE_SOURCE_DIR}/include)
