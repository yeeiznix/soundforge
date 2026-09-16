# G6 — host-only shared library for the Python ctypes parity layer.
# Links the five existing STATIC libs unchanged; default visibility exports
# the extern "C" surface. Guarded at top level by
# SF_BUILD_HOST_SHARED AND NOT CMAKE_CROSSCOMPILING: cross-compilations are
# never "host" builds and are skipped. NB this proot env reports
# ANDROID=1/SYSTEM_NAME=Android but CROSS=FALSE (native gcc, no toolchain
# file), so the target DOES build here. Edge: NDK build from an Android host
# also reports CROSS=FALSE and would build the target — accepted: the app CMake
# never add_subdirectory's this tree, and on an Android host that .so IS the
# correct host lib for ctypes (ORC-G6-18).
find_package(Threads REQUIRED)          # imported target is directory-scoped —
                                        # does NOT leak from src/core|graph|audio (ORC-G6-02)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/host_stub.cpp"
     CONTENT "/* G6 host shared-lib anchor TU — no engine logic */\n")
add_library(soundforge SHARED "${CMAKE_CURRENT_BINARY_DIR}/host_stub.cpp")  # ≥1 source required (ORC-G6-01)
target_link_libraries(soundforge PRIVATE
    -Wl,--whole-archive                                    # require ALL objects from the
    sfcore sfgraph sfdsp sfmeasure sfaudio                 # five statics (below)
    -Wl,--no-whole-archive
    Threads::Threads stdc++fs)
set_target_properties(soundforge PROPERTIES
    OUTPUT_NAME soundforge
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"         # .so lands at build/libsoundforge.so (ORC-G6-03)
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET default   # explicit: keep default visibility
    VISIBILITY_INLINES_HIDDEN OFF)
