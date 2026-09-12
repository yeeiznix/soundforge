cmake_minimum_required(VERSION 3.22)

# G0: stub library sfpower — satisfies Appendix A without logic
add_library(sfpower INTERFACE)
target_include_directories(sfpower INTERFACE ${CMAKE_SOURCE_DIR}/include)
