cmake_minimum_required(VERSION 3.16)
project(MyNAS VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

qt_add_executable(MyNAS
    src/main.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/TrayManager.h
    src/ui/TrayManager.cpp
    src/core/FileServer.h
    src/core/FileServer.cpp
)

target_include_directories(MyNAS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

target_link_libraries(MyNAS PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

set_target_properties(MyNAS PROPERTIES
    WIN32_EXECUTABLE TRUE
)
