cmake_minimum_required(VERSION 3.10)
project(NES VERSION 0.1.0)

include(CTest)

enable_testing()
add_subdirectory(tests)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#[[set(CMAKE_CXX_CLANG_TIDY clang-tidy -checks=-*,modernize-*,performance-*,misc-*)]]

add_compile_options(-Wall -Wextra -pedantic -Wno-unused-parameter)

find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(Qt5 COMPONENTS Core REQUIRED)
find_package(Qt5 COMPONENTS Gui REQUIRED)


include_directories(${Qt5Widgets_INCLUDE_DIRS})
include_directories(${Qt5Core_INCLUDE_DIRS})

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


add_library(libBus
    src/bus.cpp
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
)

add_executable(NES main.cpp)
target_link_libraries(NES
    libBus
    libCPU
    libInstruction
)

add_subdirectory(external/QHexView)

add_executable(qhex qhex.cpp
    ${QHEXVIEW_SOURCES}
    ${QHEXVIEW_HEADERS}
)

find_package(Threads)

target_link_libraries(qhex
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

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
