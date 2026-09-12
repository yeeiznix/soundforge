cmake_minimum_required(VERSION 3.22)

# G0: stub library sfmeasurement — satisfies Appendix A without logic
add_library(sfmeasurement INTERFACE)
target_include_directories(sfmeasurement INTERFACE ${CMAKE_SOURCE_DIR}/include)
