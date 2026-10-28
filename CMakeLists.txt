cmake_minimum_required(VERSION 3.16)
project(tms_editor VERSION 1.0.0 LANGUAGES CXX)

# In-app updater compares this against the GitHub release tag (issue #3).
# CI passes -DTMS_APP_VERSION=<tag> when building a v* tagged release; a
# plain local build reports 0.0.0 and never prompts for updates.
set(TMS_APP_VERSION "0.0.0" CACHE STRING "Application version reported by the updater")
configure_file(app_version.h.in app_version.h @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---- Options ----
option(BUILD_TESTS "Build tests" ON)
option(BUILD_SHELL_INTEGRATION "Build shell integration (e.g. sudoers helper)" OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

# ---- Main executable ----
add_executable(tms_editor main.cpp tmseditorwindow.cpp updater.cpp transport.cpp)

target_include_directories(tms_editor PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(tms_editor PRIVATE APP_VERSION="${TMS_APP_VERSION}")

target_link_libraries(tms_editor PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# On Windows build a true GUI app (WIN32 subsystem) so double-clicking it
# opens the window directly instead of also spawning a console terminal.
set_target_properties(tms_editor PROPERTIES WIN32_EXECUTABLE TRUE)

install(TARGETS tms_editor DESTINATION bin)

# ---- Headless byte-exact round-trip check (not installed) ----
# Usage: roundtrip_check <reference.TMS> [scratch-dir]
if(BUILD_TESTS)
    add_executable(roundtrip_check tests/roundtrip_check.cpp tmseditorwindow.cpp updater.cpp transport.cpp)
    target_include_directories(roundtrip_check PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(roundtrip_check PRIVATE APP_VERSION="${TMS_APP_VERSION}")
    target_link_libraries(roundtrip_check PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)
    install(TARGETS roundtrip_check DESTINATION bin OPTIONAL)
endif()

# ---- Packaging helpers (optional) ----
if(BUILD_SHELL_INTEGRATION AND UNIX AND NOT APPLE)
    install(FILES packaging/dlx50-tms-editor-sudoers
            DESTINATION .
            OPTIONAL)
endif()
