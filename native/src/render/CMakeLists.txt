cmake_minimum_required(VERSION 3.22)

# G0: stub library sfrender — satisfies Appendix A without logic
add_library(sfrender INTERFACE)
target_include_directories(sfrender INTERFACE ${CMAKE_SOURCE_DIR}/include)
