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

# The SDL2 + Dear ImGui frontend (screen + debugger) is optional; the emulator
# libraries and the test suite must configure and build without SDL present.
# Nothing under tests/ links it.
option(NES_BUILD_FRONTEND "Build the SDL2/Dear ImGui frontend and debugger" ON)

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
    src/rom.cpp
    src/apu.cpp
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

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
