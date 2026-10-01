cmake_minimum_required(VERSION 3.16)
project(MyNAS VERSION 0.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Windows header hygiene ───────────────────────────────────────────────────
# windows.h defines min and max as macros, which then collide with std::min,
# std::max and std::numeric_limits<T>::max(). The failure is confusing because
# MSVC reports a syntax error on the line using the standard facility rather than
# on the include that caused it. Setting this globally means a file that pulls in
# a Windows header indirectly cannot reintroduce the problem.
#
# WIN32_LEAN_AND_MEAN additionally keeps rarely-needed subsystem headers out,
# which shortens compile times.
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Qt modules ───────────────────────────────────────────────────────────────
# NetworkAuth is NOT needed here.
# Qt6::Network covers QTcpServer, QTcpSocket, QSslSocket, QSslConfiguration.
# Qt links OpenSSL automatically when Network is requested on Windows/Linux.
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Sql)

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
    src/http/RequestParser.h
    src/http/RequestParser.cpp
    src/http/MimeType.h
    src/http/MimeType.cpp
    src/http/Compression.h
    src/http/Compression.cpp
    src/http/HardwareTier.h
    src/http/HardwareTier.cpp
    src/http/AdmissionControl.h
    src/http/AdmissionControl.cpp

    # Transport: threading, sockets, TLS, framing and streaming.
    src/http/Router.h
    src/http/Connection.h
    src/http/Connection.cpp
    src/http/Worker.h
    src/http/Worker.cpp
    src/http/HttpServer.h
    src/http/HttpServer.cpp

)

# AUTOMOC is needed here now: Connection, Worker and HttpServer are QObjects.
set_target_properties(nestdrive_http PROPERTIES AUTOMOC ON)

target_link_libraries(nestdrive_http PUBLIC Qt6::Core Qt6::Network)

target_include_directories(nestdrive_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── Persistence library ──────────────────────────────────────────────────────
# Separate from nestdrive_http so the parsing tests do not drag in Qt6::Sql and
# its driver plugin.
add_library(nestdrive_db STATIC
    src/db/Database.h
    src/db/Database.cpp
    src/db/Migrations.h
    src/db/Migrations.cpp
)

target_link_libraries(nestdrive_db PUBLIC Qt6::Core Qt6::Sql)

target_include_directories(nestdrive_db PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── Media library ────────────────────────────────────────────────────────────
# Image decoding, so Qt6::Gui lives here rather than in the transport layer.
add_library(nestdrive_media STATIC
    src/media/ThumbnailCache.h
    src/media/ThumbnailCache.cpp
)

set_target_properties(nestdrive_media PROPERTIES AUTOMOC ON)
target_link_libraries(nestdrive_media PUBLIC nestdrive_db Qt6::Core Qt6::Gui)

# ── Request handlers ─────────────────────────────────────────────────────────
# Kept separate so the parser and admission tests link only nestdrive_http and do
# not pull in Qt6::Gui, Qt6::Sql or their plugins.
add_library(nestdrive_handlers STATIC
    src/handlers/FileRouter.h
    src/handlers/FileRouter.cpp
)

target_link_libraries(nestdrive_handlers PUBLIC nestdrive_http nestdrive_media)

# Web interface, embedded so the binary needs no companion files. Plain ES modules
# with no bundler: a build step would add a Node toolchain for minification that
# gzip already largely covers, and type checking we are not using.
qt_add_resources(nestdrive_handlers "nestdrive_web"
    PREFIX "/web"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/web"
    FILES
        web/index.html
        web/app.js
        web/app.css
)

# ── Source files ─────────────────────────────────────────────────────────────
qt_add_executable(MyNAS
    src/main.cpp

    src/app/Paths.h
    src/app/Paths.cpp

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
    nestdrive_handlers
    nestdrive_media
    nestdrive_http
    nestdrive_db
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Sql
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