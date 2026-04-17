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
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
)

# ── Windows: hide console window in release builds ────────────────────────────
set_target_properties(MyNAS PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# ── Optional: copy cert + key next to exe automatically during build ──────────
# Uncomment these lines if you keep cert.pem / key.pem in the project root.
#
add_custom_command(TARGET MyNAS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/cert.pem"
        "${CMAKE_SOURCE_DIR}/key.pem"
        "$<TARGET_FILE_DIR:MyNAS>"
    COMMENT "Copying TLS certificates next to executable"
)