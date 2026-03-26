cmake_minimum_required(VERSION 3.16)
project(MyNAS VERSION 0.3 LANGUAGES CXX)

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
    src/core/FileServer/FileServer.h
    src/core/FileServer/FileServer.cpp
    src/core/DdnsUpdater/DdnsUpdater.h
    src/core/DdnsUpdater/DdnsUpdater.cpp
    src/core/AppConfig/AppConfig.h
    src/core/AppConfig/AppConfig.cpp
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

# ─────────────────────────────────────────────────────────────────────────────
# Auto-copy config.ini to build folder after every build.
#
# CMAKE_CURRENT_SOURCE_DIR = project root (where CMakeLists.txt lives)
# $<TARGET_FILE_DIR:MyNAS>  = the folder containing the compiled MyNAS.exe
#
# This means:
#   - config.ini stays safely at project root (backed up, gitignored)
#   - A fresh copy lands next to MyNAS.exe on every build
#   - Deleting the build folder never loses your credentials
#   - If you update config.ini, just rebuild and the new version is copied
# ─────────────────────────────────────────────────────────────────────────────
add_custom_command(
    TARGET MyNAS POST_BUILD

    # Copy config.ini from project root → build output folder
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
        "$<TARGET_FILE_DIR:MyNAS>/config.ini"

    # Only runs if config.ini exists — won't fail on machines without it
    # (e.g. CI/CD or other developers who haven't set up their config yet)
    COMMENT "Copying config.ini to build output folder..."
)

# ─────────────────────────────────────────────────────────────────────────────
# Also copy config.ini.example so new contributors can see the template
# next to the exe and know exactly what to fill in.
# ─────────────────────────────────────────────────────────────────────────────
add_custom_command(
    TARGET MyNAS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/config.ini.example"
        "$<TARGET_FILE_DIR:MyNAS>/config.ini.example"
    COMMENT "Copying config.ini.example to build output folder..."
)
