cmake_minimum_required(VERSION 3.16)
project(tms_editor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(tms_editor main.cpp tmseditorwindow.cpp)

target_link_libraries(tms_editor PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

# On Windows build a true GUI app (WIN32 subsystem) so double-clicking it
# opens the window directly instead of also spawning a console terminal.
set_target_properties(tms_editor PROPERTIES WIN32_EXECUTABLE TRUE)

install(TARGETS tms_editor DESTINATION bin)
