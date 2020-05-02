cmake_minimum_required(VERSION 3.0.0)
project(NES VERSION 0.1.0)

include(CTest)
enable_testing()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(Qt5 COMPONENTS Core REQUIRED)
find_package(Qt5 COMPONENTS Gui REQUIRED)

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




add_library(libMemoryViewer src/memoryviewer.cpp)

target_link_libraries(libMemoryViewer
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
)

target_include_directories(libMemoryViewer
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)


add_executable(NES main.cpp)
target_link_libraries(NES
    libBus
    libCPU
)

add_executable(MV memoryw.cpp)

target_link_libraries(MV
    libMemoryViewer
    Qt5::Widgets
    Qt5::Core
    Qt5::Gui
)

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
