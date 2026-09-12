cmake_minimum_required(VERSION 3.22)

# G0: stub library sfdsp — satisfies Appendix A without logic
add_library(sfdsp INTERFACE)
target_include_directories(sfdsp INTERFACE ${CMAKE_SOURCE_DIR}/include)
