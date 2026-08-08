# googletest is pinned to a release tag rather than a branch. Two constraints:
#  - CMake 4 removed compatibility with cmake_minimum_required(VERSION < 3.5),
#    which every googletest tag through release-1.12.1 still declares, so those
#    tags cannot configure at all here.
#  - A moving ref (the old `master`, which upstream has since renamed to `main`)
#    makes the build non-reproducible and breaks outright when upstream renames.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    # Skip the update/fetch step on reconfigure so a populated build tree does
    # not need the network.
    UPDATE_DISCONNECTED TRUE
)

# Prevent overriding the parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# CONFIGURE_DEPENDS re-runs the glob on build, so a newly added test file is
# picked up without a manual re-configure.
file(GLOB_RECURSE tests_SRC CONFIGURE_DEPENDS "*.cpp")

# frontend/debugger_state.cpp is compiled INTO the test binary rather than
# linked from the frontend target. That target needs SDL and ImGui; this file
# needs neither, by design (see the comment at the top of debugger_state.h), so
# the suite still builds on a machine with no SDL installed and
# -DNES_BUILD_FRONTEND=OFF stays a supported configuration.
add_executable(tests ${tests_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/../frontend/debugger_state.cpp)

# Fixtures are located via an absolute path baked in at configure time, so the
# test binary can be run from any working directory rather than only under
# ctest's WORKING_DIRECTORY.
target_compile_definitions(tests
    PRIVATE
        NES_TEST_FILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_files"
)

target_link_libraries(tests
    gtest_main
    libBus
    libCPU
    libInstruction
)

gtest_discover_tests(tests)

# Each gtest case is registered as its own CTest test, and they are independent:
# every test that writes a fixture writes a uniquely named one, so they can all
# run at once. The suite is dominated by 512 per-opcode tests (256 opcodes x
# bus trace + final state), which parallelise perfectly.
#
# Measured on 32 cores: 38.1s serial -> 4.16s, with ~480 MB across all children.
# That 4.16s is the length of the single slowest test, so more cores will not
# help further. (The 129s -> 12.8s pair recorded here previously was ~3x these;
# re-measure rather than trust it if the numbers matter to you.)
#
#   ninja check     (or `make check`)
#
# ctest itself still defaults to serial, so `ctest -jN` remains available and
# the CTEST_PARALLEL_LEVEL environment variable is honoured as usual.
include(ProcessorCount)
ProcessorCount(NES_TEST_JOBS)
if(NES_TEST_JOBS EQUAL 0)
    set(NES_TEST_JOBS 1)
endif()

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} -j${NES_TEST_JOBS} --output-on-failure
    DEPENDS tests
    USES_TERMINAL
    COMMENT "Running the test suite on ${NES_TEST_JOBS} cores"
)

# `check` answers "did it pass". It cannot answer "how many actually ran", and
# in this repo that is the number that matters - see "Skipped is not passed" in
# CLAUDE.md. gtest_discover_tests gives every case its own process, so gtest's
# end-of-run summary is never printed, and that summary is the only place the
# SKIPPED count appears; ctest scores a skip as a pass.
#
#   ninja check-counts
#
# runs the binary itself so that summary survives, sharded via gtest's own
# GTEST_TOTAL_SHARDS to get the parallelism back. Measured on 32 cores: 35.8s
# in one process -> 5.0s across 32 shards, same 875 results. (Was 35.3s -> 4.3s
# over 872; the largest single contributor to the difference is
# 4-scanline_timing, which went from stopping at its third subtest to running
# all thirteen - 309 frames instead of 75, 0.14s to 0.54s.)
#
# UNIX-only because run_tests.sh is /bin/sh, as every other script here is.
# `check` stays the portable one, and is still the gate.
if(UNIX)
    add_custom_target(check-counts
        COMMAND ${CMAKE_COMMAND} -E env NES_TEST_BIN=$<TARGET_FILE:tests>
                ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh -j${NES_TEST_JOBS}
        DEPENDS tests
        USES_TERMINAL
        COMMENT "Running the test suite on ${NES_TEST_JOBS} shards, with the executed/skipped breakdown"
    )
endif()
