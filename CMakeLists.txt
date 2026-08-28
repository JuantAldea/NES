cmake_minimum_required(VERSION 3.15)
project(NES VERSION 0.1.0)

include(CTest)
include(FetchContent)

enable_testing()

# The default build type is "Checked": optimised, with debug info, and WITHOUT
# NDEBUG so asserts stay live.
#
# Neither stock type is right here. Debug leaves the emulator unoptimised, and
# the test suite spends most of its time emulating - the ROMs run hundreds of
# millions of bus cycles each, so this is the difference between a 16 second
# suite and a 3 second one. RelWithDebInfo and Release both define NDEBUG,
# which would silently neuter any assert-based regression test.
#
# Measured, best of three full runs on 32 cores:
#
#   -O0 (stock Debug)   16.17s
#   -O1                  4.00s
#   -O2                  3.17s
#   -O3                  2.85s   <-- chosen
#   -O3 -march=native    2.91s   slower than plain -O3, and not portable
#
# Debug, Release and the rest are untouched and still selectable.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Checked CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Checked Debug Release RelWithDebInfo MinSizeRel)

set(CMAKE_CXX_FLAGS_CHECKED "-O3 -g" CACHE STRING "C++ flags for the Checked build" FORCE)
set(CMAKE_C_FLAGS_CHECKED "-O3 -g" CACHE STRING "C flags for the Checked build" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_CHECKED "" CACHE STRING "Linker flags for the Checked build" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_CHECKED "" CACHE STRING "Shared linker flags for the Checked build" FORCE)
mark_as_advanced(CMAKE_CXX_FLAGS_CHECKED CMAKE_C_FLAGS_CHECKED CMAKE_EXE_LINKER_FLAGS_CHECKED
                 CMAKE_SHARED_LINKER_FLAGS_CHECKED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#[[set(CMAKE_CXX_CLANG_TIDY clang-tidy -checks=-*,modernize-*,performance-*,misc-*)]]

add_compile_options(-Wall -Wextra -pedantic -Wno-unused-parameter)

# Sanitizers are opt-in and off by default: they change code generation for
# every target, and the default build is the one people run constantly.
#
#   cmake -S . -B build-asan -G Ninja -DNES_SANITIZE=address,undefined
#
# NES_SANITIZE takes the raw -fsanitize= argument list, so `thread` or
# `address` alone work too. It is deliberately NOT a new build type: the
# sanitizer build wants exactly what Checked already provides (-O3 -g with
# asserts live) plus instrumentation, and duplicating the build type would
# mean keeping two copies of that rationale in sync.
#
# -O3 is kept rather than dropped to -O1. The suite is emulation-bound, and
# ASan's slowdown multiplies whatever the baseline is; at -O1 the sanitizer
# job would take roughly three times as long for no extra coverage. The frame
# pointer is forced back on because -O3 drops it, and without it ASan's stack
# traces degrade to noise.
#
# -fno-sanitize-recover=all matters more than it looks. UBSan's default is to
# print a diagnostic and CARRY ON, which exits 0 - so a CI job would go green
# while stepping on undefined behaviour on every run. Trapping makes the test
# that hits it fail, which is the entire point of running this in CI.
set(NES_SANITIZE "" CACHE STRING
    "Sanitizers to build with, as a -fsanitize= list (e.g. address,undefined). Empty disables them.")

if(NES_SANITIZE)
    message(STATUS "Building with -fsanitize=${NES_SANITIZE}")
    add_compile_options(
        -fsanitize=${NES_SANITIZE}
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=${NES_SANITIZE})
endif()

# Clang's static analyzer, the third correctness net after the ROM oracles and
# the asserts, and the only one that does not need a test to reach the code.
#
#   tests/run_scan_build.sh          (or `ninja -C build analyze`)
#
# scan-build works by interposing: it puts its own wrappers in CC/CXX, so a
# tree configured under it records /usr/lib/clang/{ccc-analyzer,c++-analyzer}
# as the compiler. Detecting that is what lets the rest of this file behave
# differently for an analysis tree without a separate option to get out of sync
# with reality - the tree either IS an analyzer tree or it is not.
if(CMAKE_CXX_COMPILER MATCHES "(ccc-analyzer|c\\+\\+-analyzer)")
    set(NES_UNDER_ANALYZER TRUE)
    message(STATUS "Configuring as a scan-build analysis tree")
else()
    set(NES_UNDER_ANALYZER FALSE)
endif()

# The SDL2 + Dear ImGui frontend (screen + debugger) is optional; the emulator
# libraries and the test suite must configure and build without SDL present.
# Nothing under tests/ links it.
#
# It defaults OFF under the analyzer. MEASURED reason, not caution: analysing
# with the frontend on produces 16 findings from ImGui itself - null
# dereferences, a division by zero, dead stores - and none of them are ours.
# scan-build's --exclude keeps them out of the bug count but cannot keep them
# out of the build log, so the only way for every analyzer line in that log to
# be actionable is not to compile ImGui at all. This is the same call as
# `target_compile_options(imgui PRIVATE -w)` further down, for the same reason.
#
# Our own frontend sources analyse clean, so -DNES_BUILD_FRONTEND=ON is a
# supported thing to do here; the default just picks the quiet side. Note
# frontend/debugger_state.cpp is analysed either way, because the test binary
# compiles it directly (see tests/CMakeLists.txt).
if(NES_UNDER_ANALYZER)
    set(NES_BUILD_FRONTEND_DEFAULT OFF)
else()
    set(NES_BUILD_FRONTEND_DEFAULT ON)
endif()

option(NES_BUILD_FRONTEND "Build the SDL2/Dear ImGui frontend and debugger" ${NES_BUILD_FRONTEND_DEFAULT})

if(NES_BUILD_FRONTEND)
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        message(STATUS "SDL2 not found - skipping the frontend target")
        set(NES_BUILD_FRONTEND OFF)
    endif()
endif()

add_library(libCPU
    src/cpu.cpp
    include/cpu.h
)

target_include_directories(libCPU
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(libInstruction
    src/instruction.cpp
)

target_include_directories(libInstruction
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(libPPU
    src/ppu.cpp
)

target_include_directories(libPPU
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)


add_library(libBus
    src/bus.cpp
    src/device.cpp
    src/rom.cpp
    src/mapper.cpp
    src/apu.cpp
    src/audio.cpp
    src/blip.cpp
)

target_include_directories(libBus
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(libBus
    libCPU
    libPPU
)

add_executable(NES main.cpp)
target_link_libraries(NES
    libBus
    libCPU
    libInstruction
)

if(NES_BUILD_FRONTEND)
    # SDL2's own config package exports SDL2::SDL2 on every distribution that
    # matters here; the variable form is the fallback for the older FindSDL2
    # modules that do not.
    if(TARGET SDL2::SDL2)
        set(NES_SDL2_LIBS SDL2::SDL2)
        set(NES_SDL2_INCLUDE_DIRS "")
    else()
        set(NES_SDL2_LIBS ${SDL2_LIBRARIES})
        set(NES_SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIRS})
    endif()

    # Dear ImGui ships no CMakeLists.txt of its own, so FetchContent only
    # downloads it and the sources are compiled into a target here.
    #
    # Pinned to a release tag for the same reasons googletest is (see
    # tests/CMakeLists.txt): a moving ref makes the build non-reproducible and
    # breaks outright when upstream renames a branch.
    #
    # The plain branch, not `-docking`, is deliberate. Docking exists to put
    # panels in separate OS windows; everything here lives in one, so taking it
    # would mean pinning a permanently-unreleased branch for a feature the
    # frontend does not use.
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.9b
        GIT_SHALLOW    TRUE
        UPDATE_DISCONNECTED TRUE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )

    target_include_directories(imgui SYSTEM
        PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
            ${NES_SDL2_INCLUDE_DIRS}
    )

    target_link_libraries(imgui PUBLIC ${NES_SDL2_LIBS})

    # Third-party sources are not held to this project's warning flags. Without
    # this, -Wall -Wextra -pedantic buries our own warnings under ImGui's.
    target_compile_options(imgui PRIVATE -w)

    add_executable(nes_frontend
        frontend/main.cpp
        frontend/debugger.cpp
        frontend/debugger_state.cpp
    )

    target_link_libraries(nes_frontend
        imgui
        libBus
        libCPU
        libInstruction
    )
endif()

# Added last so the test target inherits CMAKE_CXX_STANDARD and the warning
# flags set above, and so the library targets it links already exist.
add_subdirectory(tests)

# `ninja analyze` - the static analyzer, alongside `check` and `check-counts`.
#
#   ninja -C build analyze
#
# It does NOT analyse this build tree. scan-build has to own the configure step
# to get its wrappers recorded as the compiler, so the script drives a separate
# build-scan/ tree, exactly as the sanitizers get build-asan/ rather than
# flags bolted onto the default tree. The target is a convenience so the
# command is discoverable from the same place as the other two.
#
# Guarded on NOT being an analyzer tree, which would otherwise define a target
# whose own recipe re-enters the analyzer - `ninja analyze` inside build-scan
# would recurse until something gave way.
#
# UNIX-only for the same reason check-counts is: the script is /bin/sh.
if(UNIX AND NOT NES_UNDER_ANALYZER)
    add_custom_target(analyze
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_scan_build.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL
        COMMENT "Running clang's static analyzer over a fresh build-scan/ tree"
    )
endif()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
