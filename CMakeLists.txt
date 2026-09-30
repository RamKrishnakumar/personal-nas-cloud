cmake_minimum_required(VERSION 3.16)
project(MyNAS VERSION 0.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Qt modules ───────────────────────────────────────────────────────────────
# NetworkAuth is NOT needed here.
# Qt6::Network covers QTcpServer, QTcpSocket, QSslSocket, QSslConfiguration.
# Qt links OpenSSL automatically when Network is requested on Windows/Linux.
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# ── Pure logic library ───────────────────────────────────────────────────────
# Parsing, path resolution and (later) permission resolution live here. These
# have no socket, filesystem-state or database dependency, which is what makes
# them testable without standing up a server. Both MyNAS and the test targets
# link this, so there is exactly one copy of the logic under test.
add_library(nestdrive_http STATIC
    src/http/RangeParser.h
    src/http/RangeParser.cpp
    src/http/PathResolver.h
    src/http/PathResolver.cpp
)

target_link_libraries(nestdrive_http PUBLIC Qt6::Core)

target_include_directories(nestdrive_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── Source files ─────────────────────────────────────────────────────────────
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

# ── Include directories ───────────────────────────────────────────────────────
target_include_directories(MyNAS PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)

# ── Link libraries ────────────────────────────────────────────────────────────
# Qt6::Network includes QSslSocket / OpenSSL support.
# On Windows, deploy OpenSSL DLLs (libssl-3-x64.dll, libcrypto-3-x64.dll)
# next to the exe — Qt's windeployqt does this automatically.
target_link_libraries(MyNAS PRIVATE
    nestdrive_http
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# ── Tests ─────────────────────────────────────────────────────────────────────
# On by default for a normal source build, off when MyNAS is consumed as a
# subproject. Run with: ctest --test-dir <build-dir> --output-on-failure
option(NESTDRIVE_BUILD_TESTS "Build the NestDrive unit tests" ON)

if(NESTDRIVE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Windows: hide console window in release builds ────────────────────────────
set_target_properties(MyNAS PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# ── Copy cert + key next to the exe during build ──────────────────────────────
# AppConfig resolves relative certPath / keyPath against the executable's own
# directory, so the PEM files have to live beside MyNAS.exe rather than in the
# source tree.
#
# Both files are gitignored and are NOT present in a fresh clone — generate them
# with `sh scripts/gen-cert.sh`. The EXISTS check keeps a first build working
# before that has been done; without it, copy_if_different fails on the missing
# source and takes the whole build down. Re-run CMake configure after generating.
if(EXISTS "${CMAKE_SOURCE_DIR}/cert.pem" AND EXISTS "${CMAKE_SOURCE_DIR}/key.pem")
    add_custom_command(TARGET MyNAS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/cert.pem"
            "${CMAKE_SOURCE_DIR}/key.pem"
            "$<TARGET_FILE_DIR:MyNAS>"
        COMMENT "Copying TLS certificate and key next to executable"
    )
else()
    message(STATUS
        "MyNAS: cert.pem / key.pem not found in the source root. "
        "HTTPS will be unavailable until you run 'sh scripts/gen-cert.sh' "
        "and re-run CMake configure. Plain HTTP on the LAN is unaffected.")
endif()