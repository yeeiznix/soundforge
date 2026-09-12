cmake_minimum_required(VERSION 3.22)

# G0: stub library sfarrays — satisfies Appendix A without logic
add_library(sfarrays INTERFACE)
target_include_directories(sfarrays INTERFACE ${CMAKE_SOURCE_DIR}/include)
