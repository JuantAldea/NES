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
# Measured on 32 cores: 129s serial -> 12.8s, with ~480 MB across all children.
# That 12.8s is now the length of the single slowest test, so more cores will
# not help further.
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
