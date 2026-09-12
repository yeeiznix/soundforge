cmake_minimum_required(VERSION 3.22)

# G0: stub library sfoptimization — satisfies Appendix A without logic
add_library(sfoptimization INTERFACE)
target_include_directories(sfoptimization INTERFACE ${CMAKE_SOURCE_DIR}/include)
