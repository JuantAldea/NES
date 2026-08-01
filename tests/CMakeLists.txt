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

add_executable(tests ${tests_SRC})

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
