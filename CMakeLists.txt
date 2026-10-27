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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

add_executable(tms_editor main.cpp tmseditorwindow.cpp updater.cpp)

target_include_directories(tms_editor PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(tms_editor PRIVATE APP_VERSION="${TMS_APP_VERSION}")

target_link_libraries(tms_editor PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network)

# On Windows build a true GUI app (WIN32 subsystem) so double-clicking it
# opens the window directly instead of also spawning a console terminal.
set_target_properties(tms_editor PROPERTIES WIN32_EXECUTABLE TRUE)

install(TARGETS tms_editor DESTINATION bin)
