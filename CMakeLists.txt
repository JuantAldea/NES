cmake_minimum_required(VERSION 3.0.0)
project(NES VERSION 0.1.0)

include(CTest)
enable_testing()

find_package(Qt5 COMPONENTS Widgets REQUIRED)

add_library(libCPU src/cpu.cpp)
target_include_directories(libCPU
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(libBus src/bus.cpp)
target_include_directories(libBus
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_executable(NES main.cpp)
target_link_libraries(NES libBus libCPU)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
