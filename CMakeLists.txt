cmake_minimum_required(VERSION 3.15)
project(NES VERSION 0.1.0)

include(CTest)
include(FetchContent)

enable_testing()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#[[set(CMAKE_CXX_CLANG_TIDY clang-tidy -checks=-*,modernize-*,performance-*,misc-*)]]

add_compile_options(-Wall -Wextra -pedantic -Wno-unused-parameter)

# The Qt-based debugger (qhex) is optional; the emulator libraries and the test
# suite must configure and build without Qt present.
option(NES_BUILD_QHEX "Build the Qt hex-editor/CPU debugger" ON)

if(NES_BUILD_QHEX)
    find_package(Qt5 COMPONENTS Widgets Core Gui)
    if(NOT Qt5_FOUND)
        message(STATUS "Qt5 not found - skipping the qhex debugger target")
        set(NES_BUILD_QHEX OFF)
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

if(NES_BUILD_QHEX)
    include_directories(${Qt5Widgets_INCLUDE_DIRS})
    include_directories(${Qt5Core_INCLUDE_DIRS})

    FetchContent_Declare(
        QHexView
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/QHexView
        GIT_REPOSITORY https://github.com/Dax89/QHexView.git
        GIT_TAG        642db82e8fd649478db69f12245a08d7caef22ea
    )

    FetchContent_GetProperties(QHexView)
    FetchContent_MakeAvailable(QHexView)

    add_library(QHexView
        ${QHEXVIEW_SOURCES}
        ${QHEXVIEW_HEADERS}
    )

    find_package(Threads)

    add_executable(qhex qhex.cpp)
    target_link_libraries(qhex
        QHexView
        libBus
        libCPU
        libInstruction
        Qt5::Widgets
        Qt5::Core
        Qt5::Gui
        Threads::Threads
#[[
        -fsanitize=address
        -fno-omit-frame-pointer
]]
    )
endif()

# Added last so the test target inherits CMAKE_CXX_STANDARD and the warning
# flags set above, and so the library targets it links already exist.
add_subdirectory(tests)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
