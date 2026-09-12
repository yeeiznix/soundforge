cmake_minimum_required(VERSION 3.22)

# G0: stub library sfaudio — satisfies Appendix A without logic
add_library(sfaudio INTERFACE)
target_include_directories(sfaudio INTERFACE ${CMAKE_SOURCE_DIR}/include)
